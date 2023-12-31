/*
 * Copyright (c) 2001, 2011, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "code/icBuffer.hpp"
#include "gc_implementation/g1/bufferingOopClosure.hpp"
#include "gc_implementation/g1/concurrentG1Refine.hpp"
#include "gc_implementation/g1/concurrentG1RefineThread.hpp"
#include "gc_implementation/g1/concurrentMarkThread.inline.hpp"
#include "gc_implementation/g1/g1AllocRegion.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/g1ErgoVerbose.hpp"
#include "gc_implementation/g1/g1MarkSweep.hpp"
#include "gc_implementation/g1/g1OopClosures.inline.hpp"
#include "gc_implementation/g1/g1RemSet.inline.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "gc_implementation/g1/heapRegionSeq.inline.hpp"
#include "gc_implementation/g1/vm_operations_g1.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"
#include "memory/gcLocker.inline.hpp"
#include "memory/genOopClosures.inline.hpp"
#include "memory/generationSpec.hpp"
#include "memory/referenceProcessor.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oop.pcgc.inline.hpp"
#include "runtime/aprofiler.hpp"
#include "runtime/vmThread.hpp"

size_t G1CollectedHeap::_humongous_object_threshold_in_words = 0;

// turn it on so that the contents of the young list (scan-only /
// to-be-collected) are printed at "strategic" points before / during
// / after the collection --- this is useful for debugging
#define YOUNG_LIST_VERBOSE 0
// CURRENT STATUS
// This file is under construction.  Search for "FIXME".

// INVARIANTS/NOTES
//
// All allocation activity covered by the G1CollectedHeap interface is
// serialized by acquiring the HeapLock.  This happens in mem_allocate
// and allocate_new_tlab, which are the "entry" points to the
// allocation code from the rest of the JVM.  (Note that this does not
// apply to TLAB allocation, which is not part of this interface: it
// is done by clients of this interface.)

// Local to this file.

class RefineCardTableEntryClosure: public CardTableEntryClosure {
  SuspendibleThreadSet* _sts;
  G1RemSet* _g1rs;
  ConcurrentG1Refine* _cg1r;
  bool _concurrent;
public:
  RefineCardTableEntryClosure(SuspendibleThreadSet* sts,
                              G1RemSet* g1rs,
                              ConcurrentG1Refine* cg1r) :
    _sts(sts), _g1rs(g1rs), _cg1r(cg1r), _concurrent(true)
  {}
  bool do_card_ptr(jbyte* card_ptr, int worker_i) {
    bool oops_into_cset = _g1rs->concurrentRefineOneCard(card_ptr, worker_i, false);
    // This path is executed by the concurrent refine or mutator threads,
    // concurrently, and so we do not care if card_ptr contains references
    // that point into the collection set.
    assert(!oops_into_cset, "should be");

    if (_concurrent && _sts->should_yield()) {
      // Caller will actually yield.
      return false;
    }
    // Otherwise, we finished successfully; return true.
    return true;
  }
  void set_concurrent(bool b) { _concurrent = b; }
};


class ClearLoggedCardTableEntryClosure: public CardTableEntryClosure {
  int _calls;
  G1CollectedHeap* _g1h;
  CardTableModRefBS* _ctbs;
  int _histo[256];
public:
  ClearLoggedCardTableEntryClosure() :
    _calls(0)
  {
    _g1h = G1CollectedHeap::heap();
    _ctbs = (CardTableModRefBS*)_g1h->barrier_set();
    for (int i = 0; i < 256; i++) _histo[i] = 0;
  }
  bool do_card_ptr(jbyte* card_ptr, int worker_i) {
    if (_g1h->is_in_reserved(_ctbs->addr_for(card_ptr))) {
      _calls++;
      unsigned char* ujb = (unsigned char*)card_ptr;
      int ind = (int)(*ujb);
      _histo[ind]++;
      *card_ptr = -1;
    }
    return true;
  }
  int calls() { return _calls; }
  void print_histo() {
    gclog_or_tty->print_cr("Card table value histogram:");
    for (int i = 0; i < 256; i++) {
      if (_histo[i] != 0) {
        gclog_or_tty->print_cr("  %d: %d", i, _histo[i]);
      }
    }
  }
};

class RedirtyLoggedCardTableEntryClosure: public CardTableEntryClosure {
  int _calls;
  G1CollectedHeap* _g1h;
  CardTableModRefBS* _ctbs;
public:
  RedirtyLoggedCardTableEntryClosure() :
    _calls(0)
  {
    _g1h = G1CollectedHeap::heap();
    _ctbs = (CardTableModRefBS*)_g1h->barrier_set();
  }
  bool do_card_ptr(jbyte* card_ptr, int worker_i) {
    if (_g1h->is_in_reserved(_ctbs->addr_for(card_ptr))) {
      _calls++;
      *card_ptr = 0;
    }
    return true;
  }
  int calls() { return _calls; }
};

class RedirtyLoggedCardTableEntryFastClosure : public CardTableEntryClosure {
public:
  bool do_card_ptr(jbyte* card_ptr, int worker_i) {
    *card_ptr = CardTableModRefBS::dirty_card_val();
    return true;
  }
};

YoungList::YoungList(G1CollectedHeap* g1h)
  : _g1h(g1h), _head(NULL),
    _length(0),
    _last_sampled_rs_lengths(0),
    _survivor_head(NULL), _survivor_tail(NULL), _survivor_length(0)
{
  guarantee( check_list_empty(false), "just making sure..." );
}

void YoungList::push_region(HeapRegion *hr) {
  assert(!hr->is_young(), "should not already be young");
  assert(hr->get_next_young_region() == NULL, "cause it should!");

  hr->set_next_young_region(_head);
  _head = hr;

  hr->set_young();
  double yg_surv_rate = _g1h->g1_policy()->predict_yg_surv_rate((int)_length);
  ++_length;
}

void YoungList::add_survivor_region(HeapRegion* hr) {
  assert(hr->is_survivor(), "should be flagged as survivor region");
  assert(hr->get_next_young_region() == NULL, "cause it should!");

  hr->set_next_young_region(_survivor_head);
  if (_survivor_head == NULL) {
    _survivor_tail = hr;
  }
  _survivor_head = hr;

  ++_survivor_length;
}

void YoungList::empty_list(HeapRegion* list) {
  while (list != NULL) {
    HeapRegion* next = list->get_next_young_region();
    list->set_next_young_region(NULL);
    list->uninstall_surv_rate_group();
    list->set_not_young();
    list = next;
  }
}

void YoungList::empty_list() {
  assert(check_list_well_formed(), "young list should be well formed");

  empty_list(_head);
  _head = NULL;
  _length = 0;

  empty_list(_survivor_head);
  _survivor_head = NULL;
  _survivor_tail = NULL;
  _survivor_length = 0;

  _last_sampled_rs_lengths = 0;

  assert(check_list_empty(false), "just making sure...");
}

bool YoungList::check_list_well_formed() {
  bool ret = true;

  size_t length = 0;
  HeapRegion* curr = _head;
  HeapRegion* last = NULL;
  while (curr != NULL) {
    if (!curr->is_young()) {
      gclog_or_tty->print_cr("### YOUNG REGION "PTR_FORMAT"-"PTR_FORMAT" "
                             "incorrectly tagged (y: %d, surv: %d)",
                             curr->bottom(), curr->end(),
                             curr->is_young(), curr->is_survivor());
      ret = false;
    }
    ++length;
    last = curr;
    curr = curr->get_next_young_region();
  }
  ret = ret && (length == _length);

  if (!ret) {
    gclog_or_tty->print_cr("### YOUNG LIST seems not well formed!");
    gclog_or_tty->print_cr("###   list has %d entries, _length is %d",
                           length, _length);
  }

  return ret;
}

bool YoungList::check_list_empty(bool check_sample) {
  bool ret = true;

  if (_length != 0) {
    gclog_or_tty->print_cr("### YOUNG LIST should have 0 length, not %d",
                  _length);
    ret = false;
  }
  if (check_sample && _last_sampled_rs_lengths != 0) {
    gclog_or_tty->print_cr("### YOUNG LIST has non-zero last sampled RS lengths");
    ret = false;
  }
  if (_head != NULL) {
    gclog_or_tty->print_cr("### YOUNG LIST does not have a NULL head");
    ret = false;
  }
  if (!ret) {
    gclog_or_tty->print_cr("### YOUNG LIST does not seem empty");
  }

  return ret;
}

void
YoungList::rs_length_sampling_init() {
  _sampled_rs_lengths = 0;
  _curr               = _head;
}

bool
YoungList::rs_length_sampling_more() {
  return _curr != NULL;
}

void
YoungList::rs_length_sampling_next() {
  assert( _curr != NULL, "invariant" );
  size_t rs_length = _curr->rem_set()->occupied();

  _sampled_rs_lengths += rs_length;

  // The current region may not yet have been added to the
  // incremental collection set (it gets added when it is
  // retired as the current allocation region).
  if (_curr->in_collection_set()) {
    // Update the collection set policy information for this region
    _g1h->g1_policy()->update_incremental_cset_info(_curr, rs_length);
  }

  _curr = _curr->get_next_young_region();
  if (_curr == NULL) {
    _last_sampled_rs_lengths = _sampled_rs_lengths;
    // gclog_or_tty->print_cr("last sampled RS lengths = %d", _last_sampled_rs_lengths);
  }
}

void
YoungList::reset_auxilary_lists() {
  guarantee( is_empty(), "young list should be empty" );
  assert(check_list_well_formed(), "young list should be well formed");

  // Add survivor regions to SurvRateGroup.
  _g1h->g1_policy()->note_start_adding_survivor_regions();
  _g1h->g1_policy()->finished_recalculating_age_indexes(true /* is_survivors */);

  for (HeapRegion* curr = _survivor_head;
       curr != NULL;
       curr = curr->get_next_young_region()) {
    _g1h->g1_policy()->set_region_survivors(curr);

    // The region is a non-empty survivor so let's add it to
    // the incremental collection set for the next evacuation
    // pause.
    _g1h->g1_policy()->add_region_to_incremental_cset_rhs(curr);
  }
  _g1h->g1_policy()->note_stop_adding_survivor_regions();

  _head   = _survivor_head;
  _length = _survivor_length;
  if (_survivor_head != NULL) {
    assert(_survivor_tail != NULL, "cause it shouldn't be");
    assert(_survivor_length > 0, "invariant");
    _survivor_tail->set_next_young_region(NULL);
  }

  // Don't clear the survivor list handles until the start of
  // the next evacuation pause - we need it in order to re-tag
  // the survivor regions from this evacuation pause as 'young'
  // at the start of the next.

  _g1h->g1_policy()->finished_recalculating_age_indexes(false /* is_survivors */);

  assert(check_list_well_formed(), "young list should be well formed");
}

void YoungList::print() {
  HeapRegion* lists[] = {_head,   _survivor_head};
  const char* names[] = {"YOUNG", "SURVIVOR"};

  for (unsigned int list = 0; list < ARRAY_SIZE(lists); ++list) {
    gclog_or_tty->print_cr("%s LIST CONTENTS", names[list]);
    HeapRegion *curr = lists[list];
    if (curr == NULL)
      gclog_or_tty->print_cr("  empty");
    while (curr != NULL) {
      gclog_or_tty->print_cr("  [%08x-%08x], t: %08x, P: %08x, N: %08x, C: %08x, "
                             "age: %4d, y: %d, surv: %d",
                             curr->bottom(), curr->end(),
                             curr->top(),
                             curr->prev_top_at_mark_start(),
                             curr->next_top_at_mark_start(),
                             curr->top_at_conc_mark_count(),
                             curr->age_in_surv_rate_group_cond(),
                             curr->is_young(),
                             curr->is_survivor());
      curr = curr->get_next_young_region();
    }
  }

  gclog_or_tty->print_cr("");
}

void G1CollectedHeap::push_dirty_cards_region(HeapRegion* hr)
{
  // Claim the right to put the region on the dirty cards region list
  // by installing a self pointer.
  HeapRegion* next = hr->get_next_dirty_cards_region();
  if (next == NULL) {
    HeapRegion* res = (HeapRegion*)
      Atomic::cmpxchg_ptr(hr, hr->next_dirty_cards_region_addr(),
                          NULL);
    if (res == NULL) {
      HeapRegion* head;
      do {
        // Put the region to the dirty cards region list.
        head = _dirty_cards_region_list;
        next = (HeapRegion*)
          Atomic::cmpxchg_ptr(hr, &_dirty_cards_region_list, head);
        if (next == head) {
          assert(hr->get_next_dirty_cards_region() == hr,
                 "hr->get_next_dirty_cards_region() != hr");
          if (next == NULL) {
            // The last region in the list points to itself.
            hr->set_next_dirty_cards_region(hr);
          } else {
            hr->set_next_dirty_cards_region(next);
          }
        }
      } while (next != head);
    }
  }
}

HeapRegion* G1CollectedHeap::pop_dirty_cards_region()
{
  HeapRegion* head;
  HeapRegion* hr;
  do {
    head = _dirty_cards_region_list;
    if (head == NULL) {
      return NULL;
    }
    HeapRegion* new_head = head->get_next_dirty_cards_region();
    if (head == new_head) {
      // The last region.
      new_head = NULL;
    }
    hr = (HeapRegion*)Atomic::cmpxchg_ptr(new_head, &_dirty_cards_region_list,
                                          head);
  } while (hr != head);
  assert(hr != NULL, "invariant");
  hr->set_next_dirty_cards_region(NULL);
  return hr;
}

void G1CollectedHeap::stop_conc_gc_threads() {
  _cg1r->stop();
  _cmThread->stop();
}

#ifdef ASSERT
// A region is added to the collection set as it is retired
// so an address p can point to a region which will be in the
// collection set but has not yet been retired.  This method
// therefore is only accurate during a GC pause after all
// regions have been retired.  It is used for debugging
// to check if an nmethod has references to objects that can
// be move during a partial collection.  Though it can be
// inaccurate, it is sufficient for G1 because the conservative
// implementation of is_scavengable() for G1 will indicate that
// all nmethods must be scanned during a partial collection.
bool G1CollectedHeap::is_in_partial_collection(const void* p) {
  HeapRegion* hr = heap_region_containing(p);
  return hr != NULL && hr->in_collection_set();
}
#endif

// Returns true if the reference points to an object that
// can move in an incremental collecction.
bool G1CollectedHeap::is_scavengable(const void* p) {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1CollectorPolicy* g1p = g1h->g1_policy();
  HeapRegion* hr = heap_region_containing(p);
  if (hr == NULL) {
     // perm gen (or null)
     return false;
  } else {
    return !hr->isHumongous();
  }
}

void G1CollectedHeap::check_ct_logs_at_safepoint() {
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  CardTableModRefBS* ct_bs = (CardTableModRefBS*)barrier_set();

  // Count the dirty cards at the start.
  CountNonCleanMemRegionClosure count1(this);
  ct_bs->mod_card_iterate(&count1);
  int orig_count = count1.n();

  // First clear the logged cards.
  ClearLoggedCardTableEntryClosure clear;
  dcqs.set_closure(&clear);
  dcqs.apply_closure_to_all_completed_buffers();
  dcqs.iterate_closure_all_threads(false);
  clear.print_histo();

  // Now ensure that there's no dirty cards.
  CountNonCleanMemRegionClosure count2(this);
  ct_bs->mod_card_iterate(&count2);
  if (count2.n() != 0) {
    gclog_or_tty->print_cr("Card table has %d entries; %d originally",
                           count2.n(), orig_count);
  }
  guarantee(count2.n() == 0, "Card table should be clean.");

  RedirtyLoggedCardTableEntryClosure redirty;
  JavaThread::dirty_card_queue_set().set_closure(&redirty);
  dcqs.apply_closure_to_all_completed_buffers();
  dcqs.iterate_closure_all_threads(false);
  gclog_or_tty->print_cr("Log entries = %d, dirty cards = %d.",
                         clear.calls(), orig_count);
  guarantee(redirty.calls() == clear.calls(),
            "Or else mechanism is broken.");

  CountNonCleanMemRegionClosure count3(this);
  ct_bs->mod_card_iterate(&count3);
  if (count3.n() != orig_count) {
    gclog_or_tty->print_cr("Should have restored them all: orig = %d, final = %d.",
                           orig_count, count3.n());
    guarantee(count3.n() >= orig_count, "Should have restored them all.");
  }

  JavaThread::dirty_card_queue_set().set_closure(_refine_cte_cl);
}

// Private class members.

G1CollectedHeap* G1CollectedHeap::_g1h;

// Private methods.

HeapRegion*
G1CollectedHeap::new_region_try_secondary_free_list() {
  MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
  while (!_secondary_free_list.is_empty() || free_regions_coming()) {
    if (!_secondary_free_list.is_empty()) {
      if (G1ConcRegionFreeingVerbose) {
        gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                               "secondary_free_list has "SIZE_FORMAT" entries",
                               _secondary_free_list.length());
      }
      // It looks as if there are free regions available on the
      // secondary_free_list. Let's move them to the free_list and try
      // again to allocate from it.
      append_secondary_free_list();

      assert(!_free_list.is_empty(), "if the secondary_free_list was not "
             "empty we should have moved at least one entry to the free_list");
      HeapRegion* res = _free_list.remove_head();
      if (G1ConcRegionFreeingVerbose) {
        gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                               "allocated "HR_FORMAT" from secondary_free_list",
                               HR_FORMAT_PARAMS(res));
      }
      return res;
    }

    // Wait here until we get notifed either when (a) there are no
    // more free regions coming or (b) some regions have been moved on
    // the secondary_free_list.
    SecondaryFreeList_lock->wait(Mutex::_no_safepoint_check_flag);
  }

  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                           "could not allocate from secondary_free_list");
  }
  return NULL;
}

HeapRegion* G1CollectedHeap::new_region(size_t word_size, bool do_expand) {
  assert(!isHumongous(word_size) || word_size <= HeapRegion::GrainWords,
         "the only time we use this to allocate a humongous region is "
         "when we are allocating a single humongous region");

  HeapRegion* res;
  if (G1StressConcRegionFreeing) {
    if (!_secondary_free_list.is_empty()) {
      if (G1ConcRegionFreeingVerbose) {
        gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                               "forced to look at the secondary_free_list");
      }
      res = new_region_try_secondary_free_list();
      if (res != NULL) {
        return res;
      }
    }
  }
  res = _free_list.remove_head_or_null();
  if (res == NULL) {
    if (G1ConcRegionFreeingVerbose) {
      gclog_or_tty->print_cr("G1ConcRegionFreeing [region alloc] : "
                             "res == NULL, trying the secondary_free_list");
    }
    res = new_region_try_secondary_free_list();
  }
  if (res == NULL && do_expand) {
    ergo_verbose1(ErgoHeapSizing,
                  "attempt heap expansion",
                  ergo_format_reason("region allocation request failed")
                  ergo_format_byte("allocation request"),
                  word_size * HeapWordSize);
    if (expand(word_size * HeapWordSize)) {
      // Even though the heap was expanded, it might not have reached
      // the desired size. So, we cannot assume that the allocation
      // will succeed.
      res = _free_list.remove_head_or_null();
    }
  }
  return res;
}

size_t G1CollectedHeap::humongous_obj_allocate_find_first(size_t num_regions,
                                                          size_t word_size) {
  assert(isHumongous(word_size), "word_size should be humongous");
  assert(num_regions * HeapRegion::GrainWords >= word_size, "pre-condition");

  size_t first = G1_NULL_HRS_INDEX;
  if (num_regions == 1) {
    // Only one region to allocate, no need to go through the slower
    // path. The caller will attempt the expasion if this fails, so
    // let's not try to expand here too.
    HeapRegion* hr = new_region(word_size, false /* do_expand */);
    if (hr != NULL) {
      first = hr->hrs_index();
    } else {
      first = G1_NULL_HRS_INDEX;
    }
  } else {
    // We can't allocate humongous regions while cleanupComplete() is
    // running, since some of the regions we find to be empty might not
    // yet be added to the free list and it is not straightforward to
    // know which list they are on so that we can remove them. Note
    // that we only need to do this if we need to allocate more than
    // one region to satisfy the current humongous allocation
    // request. If we are only allocating one region we use the common
    // region allocation code (see above).
    wait_while_free_regions_coming();
    append_secondary_free_list_if_not_empty_with_lock();

    if (free_regions() >= num_regions) {
      first = _hrs.find_contiguous(num_regions);
      if (first != G1_NULL_HRS_INDEX) {
        for (size_t i = first; i < first + num_regions; ++i) {
          HeapRegion* hr = region_at(i);
          assert(hr->is_empty(), "sanity");
          assert(is_on_master_free_list(hr), "sanity");
          hr->set_pending_removal(true);
        }
        _free_list.remove_all_pending(num_regions);
      }
    }
  }
  return first;
}

HeapWord*
G1CollectedHeap::humongous_obj_allocate_initialize_regions(size_t first,
                                                           size_t num_regions,
                                                           size_t word_size) {
  assert(first != G1_NULL_HRS_INDEX, "pre-condition");
  assert(isHumongous(word_size), "word_size should be humongous");
  assert(num_regions * HeapRegion::GrainWords >= word_size, "pre-condition");

  // Index of last region in the series + 1.
  size_t last = first + num_regions;

  // We need to initialize the region(s) we just discovered. This is
  // a bit tricky given that it can happen concurrently with
  // refinement threads refining cards on these regions and
  // potentially wanting to refine the BOT as they are scanning
  // those cards (this can happen shortly after a cleanup; see CR
  // 6991377). So we have to set up the region(s) carefully and in
  // a specific order.

  // The word size sum of all the regions we will allocate.
  size_t word_size_sum = num_regions * HeapRegion::GrainWords;
  assert(word_size <= word_size_sum, "sanity");

  // This will be the "starts humongous" region.
  HeapRegion* first_hr = region_at(first);
  // The header of the new object will be placed at the bottom of
  // the first region.
  HeapWord* new_obj = first_hr->bottom();
  // This will be the new end of the first region in the series that
  // should also match the end of the last region in the seriers.
  HeapWord* new_end = new_obj + word_size_sum;
  // This will be the new top of the first region that will reflect
  // this allocation.
  HeapWord* new_top = new_obj + word_size;

  // First, we need to zero the header of the space that we will be
  // allocating. When we update top further down, some refinement
  // threads might try to scan the region. By zeroing the header we
  // ensure that any thread that will try to scan the region will
  // come across the zero klass word and bail out.
  //
  // NOTE: It would not have been correct to have used
  // CollectedHeap::fill_with_object() and make the space look like
  // an int array. The thread that is doing the allocation will
  // later update the object header to a potentially different array
  // type and, for a very short period of time, the klass and length
  // fields will be inconsistent. This could cause a refinement
  // thread to calculate the object size incorrectly.
  Copy::fill_to_words(new_obj, oopDesc::header_size(), 0);

  // We will set up the first region as "starts humongous". This
  // will also update the BOT covering all the regions to reflect
  // that there is a single object that starts at the bottom of the
  // first region.
  first_hr->set_startsHumongous(new_top, new_end);

  // Then, if there are any, we will set up the "continues
  // humongous" regions.
  HeapRegion* hr = NULL;
  for (size_t i = first + 1; i < last; ++i) {
    hr = region_at(i);
    hr->set_continuesHumongous(first_hr);
  }
  // If we have "continues humongous" regions (hr != NULL), then the
  // end of the last one should match new_end.
  assert(hr == NULL || hr->end() == new_end, "sanity");

  // Up to this point no concurrent thread would have been able to
  // do any scanning on any region in this series. All the top
  // fields still point to bottom, so the intersection between
  // [bottom,top] and [card_start,card_end] will be empty. Before we
  // update the top fields, we'll do a storestore to make sure that
  // no thread sees the update to top before the zeroing of the
  // object header and the BOT initialization.
  OrderAccess::storestore();

  // Now that the BOT and the object header have been initialized,
  // we can update top of the "starts humongous" region.
  assert(first_hr->bottom() < new_top && new_top <= first_hr->end(),
         "new_top should be in this region");
  first_hr->set_top(new_top);
  if (_hr_printer.is_active()) {
    HeapWord* bottom = first_hr->bottom();
    HeapWord* end = first_hr->orig_end();
    if ((first + 1) == last) {
      // the series has a single humongous region
      _hr_printer.alloc(G1HRPrinter::SingleHumongous, first_hr, new_top);
    } else {
      // the series has more than one humongous regions
      _hr_printer.alloc(G1HRPrinter::StartsHumongous, first_hr, end);
    }
  }

  // Now, we will update the top fields of the "continues humongous"
  // regions. The reason we need to do this is that, otherwise,
  // these regions would look empty and this will confuse parts of
  // G1. For example, the code that looks for a consecutive number
  // of empty regions will consider them empty and try to
  // re-allocate them. We can extend is_empty() to also include
  // !continuesHumongous(), but it is easier to just update the top
  // fields here. The way we set top for all regions (i.e., top ==
  // end for all regions but the last one, top == new_top for the
  // last one) is actually used when we will free up the humongous
  // region in free_humongous_region().
  hr = NULL;
  for (size_t i = first + 1; i < last; ++i) {
    hr = region_at(i);
    if ((i + 1) == last) {
      // last continues humongous region
      assert(hr->bottom() < new_top && new_top <= hr->end(),
             "new_top should fall on this region");
      hr->set_top(new_top);
      _hr_printer.alloc(G1HRPrinter::ContinuesHumongous, hr, new_top);
    } else {
      // not last one
      assert(new_top > hr->end(), "new_top should be above this region");
      hr->set_top(hr->end());
      _hr_printer.alloc(G1HRPrinter::ContinuesHumongous, hr, hr->end());
    }
  }
  // If we have continues humongous regions (hr != NULL), then the
  // end of the last one should match new_end and its top should
  // match new_top.
  assert(hr == NULL ||
         (hr->end() == new_end && hr->top() == new_top), "sanity");

  assert(first_hr->used() == word_size * HeapWordSize, "invariant");
  _summary_bytes_used += first_hr->used();
  _humongous_set.add(first_hr);

  return new_obj;
}

// If could fit into free regions w/o expansion, try.
// Otherwise, if can expand, do so.
// Otherwise, if using ex regions might help, try with ex given back.
HeapWord* G1CollectedHeap::humongous_obj_allocate(size_t word_size) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);

  verify_region_sets_optional();

  size_t num_regions =
         round_to(word_size, HeapRegion::GrainWords) / HeapRegion::GrainWords;
  size_t x_size = expansion_regions();
  size_t fs = _hrs.free_suffix();
  size_t first = humongous_obj_allocate_find_first(num_regions, word_size);
  if (first == G1_NULL_HRS_INDEX) {
    // The only thing we can do now is attempt expansion.
    if (fs + x_size >= num_regions) {
      // If the number of regions we're trying to allocate for this
      // object is at most the number of regions in the free suffix,
      // then the call to humongous_obj_allocate_find_first() above
      // should have succeeded and we wouldn't be here.
      //
      // We should only be trying to expand when the free suffix is
      // not sufficient for the object _and_ we have some expansion
      // room available.
      assert(num_regions > fs, "earlier allocation should have succeeded");

      ergo_verbose1(ErgoHeapSizing,
                    "attempt heap expansion",
                    ergo_format_reason("humongous allocation request failed")
                    ergo_format_byte("allocation request"),
                    word_size * HeapWordSize);
      if (expand((num_regions - fs) * HeapRegion::GrainBytes)) {
        // Even though the heap was expanded, it might not have
        // reached the desired size. So, we cannot assume that the
        // allocation will succeed.
        first = humongous_obj_allocate_find_first(num_regions, word_size);
      }
    }
  }

  HeapWord* result = NULL;
  if (first != G1_NULL_HRS_INDEX) {
    result =
      humongous_obj_allocate_initialize_regions(first, num_regions, word_size);
    assert(result != NULL, "it should always return a valid result");

    // A successful humongous object allocation changes the used space
    // information of the old generation so we need to recalculate the
    // sizes and update the jstat counters here.
    g1mm()->update_sizes();
  }

  verify_region_sets_optional();

  return result;
}

HeapWord* G1CollectedHeap::allocate_new_tlab(size_t word_size) {
  assert_heap_not_locked_and_not_at_safepoint();
  assert(!isHumongous(word_size), "we do not allow humongous TLABs");

  unsigned int dummy_gc_count_before;
  return attempt_allocation(word_size, &dummy_gc_count_before);
}

HeapWord*
G1CollectedHeap::mem_allocate(size_t word_size,
                              bool*  gc_overhead_limit_was_exceeded) {
  assert_heap_not_locked_and_not_at_safepoint();

  // Loop until the allocation is satisified, or unsatisfied after GC.
  for (int try_count = 1; /* we'll return */; try_count += 1) {
    unsigned int gc_count_before;

    HeapWord* result = NULL;
    if (!isHumongous(word_size)) {
      result = attempt_allocation(word_size, &gc_count_before);
    } else {
      result = attempt_allocation_humongous(word_size, &gc_count_before);
    }
    if (result != NULL) {
      return result;
    }

    // Create the garbage collection operation...
    VM_G1CollectForAllocation op(gc_count_before, word_size);
    // ...and get the VM thread to execute it.
    VMThread::execute(&op);

    if (op.prologue_succeeded() && op.pause_succeeded()) {
      // If the operation was successful we'll return the result even
      // if it is NULL. If the allocation attempt failed immediately
      // after a Full GC, it's unlikely we'll be able to allocate now.
      HeapWord* result = op.result();
      if (result != NULL && !isHumongous(word_size)) {
        // Allocations that take place on VM operations do not do any
        // card dirtying and we have to do it here. We only have to do
        // this for non-humongous allocations, though.
        dirty_young_block(result, word_size);
      }
      return result;
    } else {
      assert(op.result() == NULL,
             "the result should be NULL if the VM op did not succeed");
    }

    // Give a warning if we seem to be looping forever.
    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      warning("G1CollectedHeap::mem_allocate retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

HeapWord* G1CollectedHeap::attempt_allocation_slow(size_t word_size,
                                           unsigned int *gc_count_before_ret) {
  // Make sure you read the note in attempt_allocation_humongous().

  assert_heap_not_locked_and_not_at_safepoint();
  assert(!isHumongous(word_size), "attempt_allocation_slow() should not "
         "be called for humongous allocation requests");

  // We should only get here after the first-level allocation attempt
  // (attempt_allocation()) failed to allocate.

  // We will loop until a) we manage to successfully perform the
  // allocation or b) we successfully schedule a collection which
  // fails to perform the allocation. b) is the only case when we'll
  // return NULL.
  HeapWord* result = NULL;
  for (int try_count = 1; /* we'll return */; try_count += 1) {
    bool should_try_gc;
    unsigned int gc_count_before;

    {
      MutexLockerEx x(Heap_lock);

      result = _mutator_alloc_region.attempt_allocation_locked(word_size,
                                                      false /* bot_updates */);
      if (result != NULL) {
        return result;
      }

      // If we reach here, attempt_allocation_locked() above failed to
      // allocate a new region. So the mutator alloc region should be NULL.
      assert(_mutator_alloc_region.get() == NULL, "only way to get here");

      if (GC_locker::is_active_and_needs_gc()) {
        if (g1_policy()->can_expand_young_list()) {
          // No need for an ergo verbose message here,
          // can_expand_young_list() does this when it returns true.
          result = _mutator_alloc_region.attempt_allocation_force(word_size,
                                                      false /* bot_updates */);
          if (result != NULL) {
            return result;
          }
        }
        should_try_gc = false;
      } else {
        // Read the GC count while still holding the Heap_lock.
        gc_count_before = SharedHeap::heap()->total_collections();
        should_try_gc = true;
      }
    }

    if (should_try_gc) {
      bool succeeded;
      result = do_collection_pause(word_size, gc_count_before, &succeeded);
      if (result != NULL) {
        assert(succeeded, "only way to get back a non-NULL result");
        return result;
      }

      if (succeeded) {
        // If we get here we successfully scheduled a collection which
        // failed to allocate. No point in trying to allocate
        // further. We'll just return NULL.
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = SharedHeap::heap()->total_collections();
        return NULL;
      }
    } else {
      GC_locker::stall_until_clear();
    }

    // We can reach here if we were unsuccessul in scheduling a
    // collection (because another thread beat us to it) or if we were
    // stalled due to the GC locker. In either can we should retry the
    // allocation attempt in case another thread successfully
    // performed a collection and reclaimed enough space. We do the
    // first attempt (without holding the Heap_lock) here and the
    // follow-on attempt will be at the start of the next loop
    // iteration (after taking the Heap_lock).
    result = _mutator_alloc_region.attempt_allocation(word_size,
                                                      false /* bot_updates */);
    if (result != NULL ){
      return result;
    }

    // Give a warning if we seem to be looping forever.
    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      warning("G1CollectedHeap::attempt_allocation_slow() "
              "retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

HeapWord* G1CollectedHeap::attempt_allocation_humongous(size_t word_size,
                                          unsigned int * gc_count_before_ret) {
  // The structure of this method has a lot of similarities to
  // attempt_allocation_slow(). The reason these two were not merged
  // into a single one is that such a method would require several "if
  // allocation is not humongous do this, otherwise do that"
  // conditional paths which would obscure its flow. In fact, an early
  // version of this code did use a unified method which was harder to
  // follow and, as a result, it had subtle bugs that were hard to
  // track down. So keeping these two methods separate allows each to
  // be more readable. It will be good to keep these two in sync as
  // much as possible.

  assert_heap_not_locked_and_not_at_safepoint();
  assert(isHumongous(word_size), "attempt_allocation_humongous() "
         "should only be called for humongous allocations");

  // We will loop until a) we manage to successfully perform the
  // allocation or b) we successfully schedule a collection which
  // fails to perform the allocation. b) is the only case when we'll
  // return NULL.
  HeapWord* result = NULL;
  for (int try_count = 1; /* we'll return */; try_count += 1) {
    bool should_try_gc;
    unsigned int gc_count_before;

    {
      MutexLockerEx x(Heap_lock);

      // Given that humongous objects are not allocated in young
      // regions, we'll first try to do the allocation without doing a
      // collection hoping that there's enough space in the heap.
      result = humongous_obj_allocate(word_size);
      if (result != NULL) {
        return result;
      }

      if (GC_locker::is_active_and_needs_gc()) {
        should_try_gc = false;
      } else {
        // Read the GC count while still holding the Heap_lock.
        gc_count_before = SharedHeap::heap()->total_collections();
        should_try_gc = true;
      }
    }

    if (should_try_gc) {
      // If we failed to allocate the humongous object, we should try to
      // do a collection pause (if we're allowed) in case it reclaims
      // enough space for the allocation to succeed after the pause.

      bool succeeded;
      result = do_collection_pause(word_size, gc_count_before, &succeeded);
      if (result != NULL) {
        assert(succeeded, "only way to get back a non-NULL result");
        return result;
      }

      if (succeeded) {
        // If we get here we successfully scheduled a collection which
        // failed to allocate. No point in trying to allocate
        // further. We'll just return NULL.
        MutexLockerEx x(Heap_lock);
        *gc_count_before_ret = SharedHeap::heap()->total_collections();
        return NULL;
      }
    } else {
      GC_locker::stall_until_clear();
    }

    // We can reach here if we were unsuccessul in scheduling a
    // collection (because another thread beat us to it) or if we were
    // stalled due to the GC locker. In either can we should retry the
    // allocation attempt in case another thread successfully
    // performed a collection and reclaimed enough space.  Give a
    // warning if we seem to be looping forever.

    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      warning("G1CollectedHeap::attempt_allocation_humongous() "
              "retries %d times", try_count);
    }
  }

  ShouldNotReachHere();
  return NULL;
}

HeapWord* G1CollectedHeap::attempt_allocation_at_safepoint(size_t word_size,
                                       bool expect_null_mutator_alloc_region) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  assert(_mutator_alloc_region.get() == NULL ||
                                             !expect_null_mutator_alloc_region,
         "the current alloc region was unexpectedly found to be non-NULL");

  if (!isHumongous(word_size)) {
    return _mutator_alloc_region.attempt_allocation_locked(word_size,
                                                      false /* bot_updates */);
  } else {
    return humongous_obj_allocate(word_size);
  }

  ShouldNotReachHere();
}

class PostMCRemSetClearClosure: public HeapRegionClosure {
  ModRefBarrierSet* _mr_bs;
public:
  PostMCRemSetClearClosure(ModRefBarrierSet* mr_bs) : _mr_bs(mr_bs) {}
  bool doHeapRegion(HeapRegion* r) {
    r->reset_gc_time_stamp();
    if (r->continuesHumongous())
      return false;
    HeapRegionRemSet* hrrs = r->rem_set();
    if (hrrs != NULL) hrrs->clear();
    // You might think here that we could clear just the cards
    // corresponding to the used region.  But no: if we leave a dirty card
    // in a region we might allocate into, then it would prevent that card
    // from being enqueued, and cause it to be missed.
    // Re: the performance cost: we shouldn't be doing full GC anyway!
    _mr_bs->clear(MemRegion(r->bottom(), r->end()));
    return false;
  }
};


class PostMCRemSetInvalidateClosure: public HeapRegionClosure {
  ModRefBarrierSet* _mr_bs;
public:
  PostMCRemSetInvalidateClosure(ModRefBarrierSet* mr_bs) : _mr_bs(mr_bs) {}
  bool doHeapRegion(HeapRegion* r) {
    if (r->continuesHumongous()) return false;
    if (r->used_region().word_size() != 0) {
      _mr_bs->invalidate(r->used_region(), true /*whole heap*/);
    }
    return false;
  }
};

class RebuildRSOutOfRegionClosure: public HeapRegionClosure {
  G1CollectedHeap*   _g1h;
  UpdateRSOopClosure _cl;
  int                _worker_i;
public:
  RebuildRSOutOfRegionClosure(G1CollectedHeap* g1, int worker_i = 0) :
    _cl(g1->g1_rem_set(), worker_i),
    _worker_i(worker_i),
    _g1h(g1)
  { }

  bool doHeapRegion(HeapRegion* r) {
    if (!r->continuesHumongous()) {
      _cl.set_from(r);
      r->oop_iterate(&_cl);
    }
    return false;
  }
};

class ParRebuildRSTask: public AbstractGangTask {
  G1CollectedHeap* _g1;
public:
  ParRebuildRSTask(G1CollectedHeap* g1)
    : AbstractGangTask("ParRebuildRSTask"),
      _g1(g1)
  { }

  void work(int i) {
    RebuildRSOutOfRegionClosure rebuild_rs(_g1, i);
    _g1->heap_region_par_iterate_chunked(&rebuild_rs, i,
                                         HeapRegion::RebuildRSClaimValue);
  }
};

class PostCompactionPrinterClosure: public HeapRegionClosure {
private:
  G1HRPrinter* _hr_printer;
public:
  bool doHeapRegion(HeapRegion* hr) {
    assert(!hr->is_young(), "not expecting to find young regions");
    // We only generate output for non-empty regions.
    if (!hr->is_empty()) {
      if (!hr->isHumongous()) {
        _hr_printer->post_compaction(hr, G1HRPrinter::Old);
      } else if (hr->startsHumongous()) {
        if (hr->capacity() == HeapRegion::GrainBytes) {
          // single humongous region
          _hr_printer->post_compaction(hr, G1HRPrinter::SingleHumongous);
        } else {
          _hr_printer->post_compaction(hr, G1HRPrinter::StartsHumongous);
        }
      } else {
        assert(hr->continuesHumongous(), "only way to get here");
        _hr_printer->post_compaction(hr, G1HRPrinter::ContinuesHumongous);
      }
    }
    return false;
  }

  PostCompactionPrinterClosure(G1HRPrinter* hr_printer)
    : _hr_printer(hr_printer) { }
};

bool G1CollectedHeap::do_collection(bool explicit_gc,
                                    bool clear_all_soft_refs,
                                    size_t word_size) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  if (GC_locker::check_active_before_gc()) {
    return false;
  }

  SvcGCMarker sgcm(SvcGCMarker::FULL);
  ResourceMark rm;

  if (PrintHeapAtGC) {
    Universe::print_heap_before_gc();
  }

  verify_region_sets_optional();

  const bool do_clear_all_soft_refs = clear_all_soft_refs ||
                           collector_policy()->should_clear_all_soft_refs();

  ClearedAllSoftRefs casr(do_clear_all_soft_refs, collector_policy());

  {
    IsGCActiveMark x;

    // Timing
    bool system_gc = (gc_cause() == GCCause::_java_lang_system_gc);
    assert(!system_gc || explicit_gc, "invariant");
    gclog_or_tty->date_stamp(PrintGC && PrintGCDateStamps);
    TraceCPUTime tcpu(PrintGCDetails, true, gclog_or_tty);
    TraceTime t(system_gc ? "Full GC (System.gc())" : "Full GC",
                PrintGC, true, gclog_or_tty);

    TraceCollectorStats tcs(g1mm()->full_collection_counters());
    TraceMemoryManagerStats tms(true /* fullGC */, gc_cause());

    double start = os::elapsedTime();
    g1_policy()->record_full_collection_start();

    wait_while_free_regions_coming();
    append_secondary_free_list_if_not_empty_with_lock();

    gc_prologue(true);
    increment_total_collections(true /* full gc */);

    size_t g1h_prev_used = used();
    assert(used() == recalculate_used(), "Should be equal");

    if (VerifyBeforeGC && total_collections() >= VerifyGCStartAt) {
      HandleMark hm;  // Discard invalid handles created during verification
      gclog_or_tty->print(" VerifyBeforeGC:");
      prepare_for_verify();
      Universe::verify(/* allow dirty */ true,
                       /* silent      */ false,
                       /* option      */ VerifyOption_G1UsePrevMarking);

    }
    pre_full_gc_dump();

    COMPILER2_PRESENT(DerivedPointerTable::clear());

    // Disable discovery and empty the discovered lists
    // for the CM ref processor.
    ref_processor_cm()->disable_discovery();
    ref_processor_cm()->abandon_partial_discovery();
    ref_processor_cm()->verify_no_references_recorded();

    // Abandon current iterations of concurrent marking and concurrent
    // refinement, if any are in progress.
    concurrent_mark()->abort();

    // Make sure we'll choose a new allocation region afterwards.
    release_mutator_alloc_region();
    abandon_gc_alloc_regions();
    g1_rem_set()->cleanupHRRS();
    tear_down_region_lists();

    // We should call this after we retire any currently active alloc
    // regions so that all the ALLOC / RETIRE events are generated
    // before the start GC event.
    _hr_printer.start_gc(true /* full */, (size_t) total_collections());

    // We may have added regions to the current incremental collection
    // set between the last GC or pause and now. We need to clear the
    // incremental collection set and then start rebuilding it afresh
    // after this full GC.
    abandon_collection_set(g1_policy()->inc_cset_head());
    g1_policy()->clear_incremental_cset();
    g1_policy()->stop_incremental_cset_building();

    empty_young_list();
    g1_policy()->set_full_young_gcs(true);

    // See the comments in g1CollectedHeap.hpp and
    // G1CollectedHeap::ref_processing_init() about
    // how reference processing currently works in G1.

    // Temporarily make discovery by the STW ref processor single threaded (non-MT).
    ReferenceProcessorMTDiscoveryMutator stw_rp_disc_ser(ref_processor_stw(), false);

    // Temporarily clear the STW ref processor's _is_alive_non_header field.
    ReferenceProcessorIsAliveMutator stw_rp_is_alive_null(ref_processor_stw(), NULL);

    ref_processor_stw()->enable_discovery(true /*verify_disabled*/, true /*verify_no_refs*/);
    ref_processor_stw()->setup_policy(do_clear_all_soft_refs);

    // Do collection work
    {
      HandleMark hm;  // Discard invalid handles created during gc
      G1MarkSweep::invoke_at_safepoint(ref_processor_stw(), do_clear_all_soft_refs);
    }

    assert(free_regions() == 0, "we should not have added any free regions");
    rebuild_region_lists();

    _summary_bytes_used = recalculate_used();

    // Enqueue any discovered reference objects that have
    // not been removed from the discovered lists.
    ref_processor_stw()->enqueue_discovered_references();

    COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

    MemoryService::track_memory_usage();

    if (VerifyAfterGC && total_collections() >= VerifyGCStartAt) {
      HandleMark hm;  // Discard invalid handles created during verification
      gclog_or_tty->print(" VerifyAfterGC:");
      prepare_for_verify();
      Universe::verify(/* allow dirty */ false,
                       /* silent      */ false,
                       /* option      */ VerifyOption_G1UsePrevMarking);

    }

    assert(!ref_processor_stw()->discovery_enabled(), "Postcondition");
    ref_processor_stw()->verify_no_references_recorded();

    // Note: since we've just done a full GC, concurrent
    // marking is no longer active. Therefore we need not
    // re-enable reference discovery for the CM ref processor.
    // That will be done at the start of the next marking cycle.
    assert(!ref_processor_cm()->discovery_enabled(), "Postcondition");
    ref_processor_cm()->verify_no_references_recorded();

    reset_gc_time_stamp();
    // Since everything potentially moved, we will clear all remembered
    // sets, and clear all cards.  Later we will rebuild remebered
    // sets. We will also reset the GC time stamps of the regions.
    PostMCRemSetClearClosure rs_clear(mr_bs());
    heap_region_iterate(&rs_clear);

    // Resize the heap if necessary.
    resize_if_necessary_after_full_collection(explicit_gc ? 0 : word_size);

    if (_hr_printer.is_active()) {
      // We should do this after we potentially resize the heap so
      // that all the COMMIT / UNCOMMIT events are generated before
      // the end GC event.

      PostCompactionPrinterClosure cl(hr_printer());
      heap_region_iterate(&cl);

      _hr_printer.end_gc(true /* full */, (size_t) total_collections());
    }

    if (_cg1r->use_cache()) {
      _cg1r->clear_and_record_card_counts();
      _cg1r->clear_hot_cache();
    }

    // Rebuild remembered sets of all regions.

    if (G1CollectedHeap::use_parallel_gc_threads()) {
      ParRebuildRSTask rebuild_rs_task(this);
      assert(check_heap_region_claim_values(
             HeapRegion::InitialClaimValue), "sanity check");
      set_par_threads(workers()->total_workers());
      workers()->run_task(&rebuild_rs_task);
      set_par_threads(0);
      assert(check_heap_region_claim_values(
             HeapRegion::RebuildRSClaimValue), "sanity check");
      reset_heap_region_claim_values();
    } else {
      RebuildRSOutOfRegionClosure rebuild_rs(this);
      heap_region_iterate(&rebuild_rs);
    }

    if (PrintGC) {
      print_size_transition(gclog_or_tty, g1h_prev_used, used(), capacity());
    }

    if (true) { // FIXME
      // Ask the permanent generation to adjust size for full collections
      perm()->compute_new_size();
    }

    // Start a new incremental collection set for the next pause
    assert(g1_policy()->collection_set() == NULL, "must be");
    g1_policy()->start_incremental_cset_building();

    // Clear the _cset_fast_test bitmap in anticipation of adding
    // regions to the incremental collection set for the next
    // evacuation pause.
    clear_cset_fast_test();

    init_mutator_alloc_region();

    double end = os::elapsedTime();
    g1_policy()->record_full_collection_end();

#ifdef TRACESPINNING
    ParallelTaskTerminator::print_termination_counts();
#endif

    gc_epilogue(true);

    // Discard all rset updates
    JavaThread::dirty_card_queue_set().abandon_logs();
    assert(!G1DeferredRSUpdate
           || (G1DeferredRSUpdate && (dirty_card_queue_set().completed_buffers_num() == 0)), "Should not be any");
  }

  _young_list->reset_sampled_info();
  // At this point there should be no regions in the
  // entire heap tagged as young.
  assert( check_young_list_empty(true /* check_heap */),
    "young list should be empty at this point");

  // Update the number of full collections that have been completed.
  increment_full_collections_completed(false /* concurrent */);

  _hrs.verify_optional();
  verify_region_sets_optional();

  if (PrintHeapAtGC) {
    Universe::print_heap_after_gc();
  }
  g1mm()->update_sizes();
  post_full_gc_dump();

  return true;
}

void G1CollectedHeap::do_full_collection(bool clear_all_soft_refs) {
  // do_collection() will return whether it succeeded in performing
  // the GC. Currently, there is no facility on the
  // do_full_collection() API to notify the caller than the collection
  // did not succeed (e.g., because it was locked out by the GC
  // locker). So, right now, we'll ignore the return value.
  bool dummy = do_collection(true,                /* explicit_gc */
                             clear_all_soft_refs,
                             0                    /* word_size */);
}

// This code is mostly copied from TenuredGeneration.
void
G1CollectedHeap::
resize_if_necessary_after_full_collection(size_t word_size) {
  assert(MinHeapFreeRatio <= MaxHeapFreeRatio, "sanity check");

  // Include the current allocation, if any, and bytes that will be
  // pre-allocated to support collections, as "used".
  const size_t used_after_gc = used();
  const size_t capacity_after_gc = capacity();
  const size_t free_after_gc = capacity_after_gc - used_after_gc;

  // This is enforced in arguments.cpp.
  assert(MinHeapFreeRatio <= MaxHeapFreeRatio,
         "otherwise the code below doesn't make sense");

  // We don't have floating point command-line arguments
  const double minimum_free_percentage = (double) MinHeapFreeRatio / 100.0;
  const double maximum_used_percentage = 1.0 - minimum_free_percentage;
  const double maximum_free_percentage = (double) MaxHeapFreeRatio / 100.0;
  const double minimum_used_percentage = 1.0 - maximum_free_percentage;

  const size_t min_heap_size = collector_policy()->min_heap_byte_size();
  const size_t max_heap_size = collector_policy()->max_heap_byte_size();

  // We have to be careful here as these two calculations can overflow
  // 32-bit size_t's.
  double used_after_gc_d = (double) used_after_gc;
  double minimum_desired_capacity_d = used_after_gc_d / maximum_used_percentage;
  double maximum_desired_capacity_d = used_after_gc_d / minimum_used_percentage;

  // Let's make sure that they are both under the max heap size, which
  // by default will make them fit into a size_t.
  double desired_capacity_upper_bound = (double) max_heap_size;
  minimum_desired_capacity_d = MIN2(minimum_desired_capacity_d,
                                    desired_capacity_upper_bound);
  maximum_desired_capacity_d = MIN2(maximum_desired_capacity_d,
                                    desired_capacity_upper_bound);

  // We can now safely turn them into size_t's.
  size_t minimum_desired_capacity = (size_t) minimum_desired_capacity_d;
  size_t maximum_desired_capacity = (size_t) maximum_desired_capacity_d;

  // This assert only makes sense here, before we adjust them
  // with respect to the min and max heap size.
  assert(minimum_desired_capacity <= maximum_desired_capacity,
         err_msg("minimum_desired_capacity = "SIZE_FORMAT", "
                 "maximum_desired_capacity = "SIZE_FORMAT,
                 minimum_desired_capacity, maximum_desired_capacity));

  // Should not be greater than the heap max size. No need to adjust
  // it with respect to the heap min size as it's a lower bound (i.e.,
  // we'll try to make the capacity larger than it, not smaller).
  minimum_desired_capacity = MIN2(minimum_desired_capacity, max_heap_size);
  // Should not be less than the heap min size. No need to adjust it
  // with respect to the heap max size as it's an upper bound (i.e.,
  // we'll try to make the capacity smaller than it, not greater).
  maximum_desired_capacity =  MAX2(maximum_desired_capacity, min_heap_size);

  if (capacity_after_gc < minimum_desired_capacity) {
    // Don't expand unless it's significant
    size_t expand_bytes = minimum_desired_capacity - capacity_after_gc;
    ergo_verbose4(ErgoHeapSizing,
                  "attempt heap expansion",
                  ergo_format_reason("capacity lower than "
                                     "min desired capacity after Full GC")
                  ergo_format_byte("capacity")
                  ergo_format_byte("occupancy")
                  ergo_format_byte_perc("min desired capacity"),
                  capacity_after_gc, used_after_gc,
                  minimum_desired_capacity, (double) MinHeapFreeRatio);
    expand(expand_bytes);

    // No expansion, now see if we want to shrink
  } else if (capacity_after_gc > maximum_desired_capacity) {
    // Capacity too large, compute shrinking size
    size_t shrink_bytes = capacity_after_gc - maximum_desired_capacity;
    ergo_verbose4(ErgoHeapSizing,
                  "attempt heap shrinking",
                  ergo_format_reason("capacity higher than "
                                     "max desired capacity after Full GC")
                  ergo_format_byte("capacity")
                  ergo_format_byte("occupancy")
                  ergo_format_byte_perc("max desired capacity"),
                  capacity_after_gc, used_after_gc,
                  maximum_desired_capacity, (double) MaxHeapFreeRatio);
    shrink(shrink_bytes);
  }
}


HeapWord*
G1CollectedHeap::satisfy_failed_allocation(size_t word_size,
                                           bool* succeeded) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  *succeeded = true;
  // Let's attempt the allocation first.
  HeapWord* result =
    attempt_allocation_at_safepoint(word_size,
                                 false /* expect_null_mutator_alloc_region */);
  if (result != NULL) {
    assert(*succeeded, "sanity");
    return result;
  }

  // In a G1 heap, we're supposed to keep allocation from failing by
  // incremental pauses.  Therefore, at least for now, we'll favor
  // expansion over collection.  (This might change in the future if we can
  // do something smarter than full collection to satisfy a failed alloc.)
  result = expand_and_allocate(word_size);
  if (result != NULL) {
    assert(*succeeded, "sanity");
    return result;
  }

  // Expansion didn't work, we'll try to do a Full GC.
  bool gc_succeeded = do_collection(false, /* explicit_gc */
                                    false, /* clear_all_soft_refs */
                                    word_size);
  if (!gc_succeeded) {
    *succeeded = false;
    return NULL;
  }

  // Retry the allocation
  result = attempt_allocation_at_safepoint(word_size,
                                  true /* expect_null_mutator_alloc_region */);
  if (result != NULL) {
    assert(*succeeded, "sanity");
    return result;
  }

  // Then, try a Full GC that will collect all soft references.
  gc_succeeded = do_collection(false, /* explicit_gc */
                               true,  /* clear_all_soft_refs */
                               word_size);
  if (!gc_succeeded) {
    *succeeded = false;
    return NULL;
  }

  // Retry the allocation once more
  result = attempt_allocation_at_safepoint(word_size,
                                  true /* expect_null_mutator_alloc_region */);
  if (result != NULL) {
    assert(*succeeded, "sanity");
    return result;
  }

  assert(!collector_policy()->should_clear_all_soft_refs(),
         "Flag should have been handled and cleared prior to this point");

  // What else?  We might try synchronous finalization later.  If the total
  // space available is large enough for the allocation, then a more
  // complete compaction phase than we've tried so far might be
  // appropriate.
  assert(*succeeded, "sanity");
  return NULL;
}

// Attempting to expand the heap sufficiently
// to support an allocation of the given "word_size".  If
// successful, perform the allocation and return the address of the
// allocated block, or else "NULL".

HeapWord* G1CollectedHeap::expand_and_allocate(size_t word_size) {
  assert_at_safepoint(true /* should_be_vm_thread */);

  verify_region_sets_optional();

  size_t expand_bytes = MAX2(word_size * HeapWordSize, MinHeapDeltaBytes);
  ergo_verbose1(ErgoHeapSizing,
                "attempt heap expansion",
                ergo_format_reason("allocation request failed")
                ergo_format_byte("allocation request"),
                word_size * HeapWordSize);
  if (expand(expand_bytes)) {
    _hrs.verify_optional();
    verify_region_sets_optional();
    return attempt_allocation_at_safepoint(word_size,
                                 false /* expect_null_mutator_alloc_region */);
  }
  return NULL;
}

void G1CollectedHeap::update_committed_space(HeapWord* old_end,
                                             HeapWord* new_end) {
  assert(old_end != new_end, "don't call this otherwise");
  assert((HeapWord*) _g1_storage.high() == new_end, "invariant");

  // Update the committed mem region.
  _g1_committed.set_end(new_end);
  // Tell the card table about the update.
  Universe::heap()->barrier_set()->resize_covered_region(_g1_committed);
  // Tell the BOT about the update.
  _bot_shared->resize(_g1_committed.word_size());
}

bool G1CollectedHeap::expand(size_t expand_bytes) {
  size_t old_mem_size = _g1_storage.committed_size();
  size_t aligned_expand_bytes = ReservedSpace::page_align_size_up(expand_bytes);
  aligned_expand_bytes = align_size_up(aligned_expand_bytes,
                                       HeapRegion::GrainBytes);
  ergo_verbose2(ErgoHeapSizing,
                "expand the heap",
                ergo_format_byte("requested expansion amount")
                ergo_format_byte("attempted expansion amount"),
                expand_bytes, aligned_expand_bytes);

  // First commit the memory.
  HeapWord* old_end = (HeapWord*) _g1_storage.high();
  bool successful = _g1_storage.expand_by(aligned_expand_bytes);
  if (successful) {
    // Then propagate this update to the necessary data structures.
    HeapWord* new_end = (HeapWord*) _g1_storage.high();
    update_committed_space(old_end, new_end);

    FreeRegionList expansion_list("Local Expansion List");
    MemRegion mr = _hrs.expand_by(old_end, new_end, &expansion_list);
    assert(mr.start() == old_end, "post-condition");
    // mr might be a smaller region than what was requested if
    // expand_by() was unable to allocate the HeapRegion instances
    assert(mr.end() <= new_end, "post-condition");

    size_t actual_expand_bytes = mr.byte_size();
    assert(actual_expand_bytes <= aligned_expand_bytes, "post-condition");
    assert(actual_expand_bytes == expansion_list.total_capacity_bytes(),
           "post-condition");
    if (actual_expand_bytes < aligned_expand_bytes) {
      // We could not expand _hrs to the desired size. In this case we
      // need to shrink the committed space accordingly.
      assert(mr.end() < new_end, "invariant");

      size_t diff_bytes = aligned_expand_bytes - actual_expand_bytes;
      // First uncommit the memory.
      _g1_storage.shrink_by(diff_bytes);
      // Then propagate this update to the necessary data structures.
      update_committed_space(new_end, mr.end());
    }
    _free_list.add_as_tail(&expansion_list);

    if (_hr_printer.is_active()) {
      HeapWord* curr = mr.start();
      while (curr < mr.end()) {
        HeapWord* curr_end = curr + HeapRegion::GrainWords;
        _hr_printer.commit(curr, curr_end);
        curr = curr_end;
      }
      assert(curr == mr.end(), "post-condition");
    }
    g1_policy()->record_new_heap_size(n_regions());
  } else {
    ergo_verbose0(ErgoHeapSizing,
                  "did not expand the heap",
                  ergo_format_reason("heap expansion operation failed"));
    // The expansion of the virtual storage space was unsuccessful.
    // Let's see if it was because we ran out of swap.
    if (G1ExitOnExpansionFailure &&
        _g1_storage.uncommitted_size() >= aligned_expand_bytes) {
      // We had head room...
      vm_exit_out_of_memory(aligned_expand_bytes, "G1 heap expansion");
    }
  }
  return successful;
}

void G1CollectedHeap::shrink_helper(size_t shrink_bytes) {
  size_t old_mem_size = _g1_storage.committed_size();
  size_t aligned_shrink_bytes =
    ReservedSpace::page_align_size_down(shrink_bytes);
  aligned_shrink_bytes = align_size_down(aligned_shrink_bytes,
                                         HeapRegion::GrainBytes);
  size_t num_regions_deleted = 0;
  MemRegion mr = _hrs.shrink_by(aligned_shrink_bytes, &num_regions_deleted);
  HeapWord* old_end = (HeapWord*) _g1_storage.high();
  assert(mr.end() == old_end, "post-condition");

  ergo_verbose3(ErgoHeapSizing,
                "shrink the heap",
                ergo_format_byte("requested shrinking amount")
                ergo_format_byte("aligned shrinking amount")
                ergo_format_byte("attempted shrinking amount"),
                shrink_bytes, aligned_shrink_bytes, mr.byte_size());
  if (mr.byte_size() > 0) {
    if (_hr_printer.is_active()) {
      HeapWord* curr = mr.end();
      while (curr > mr.start()) {
        HeapWord* curr_end = curr;
        curr -= HeapRegion::GrainWords;
        _hr_printer.uncommit(curr, curr_end);
      }
      assert(curr == mr.start(), "post-condition");
    }

    _g1_storage.shrink_by(mr.byte_size());
    HeapWord* new_end = (HeapWord*) _g1_storage.high();
    assert(mr.start() == new_end, "post-condition");

    _expansion_regions += num_regions_deleted;
    update_committed_space(old_end, new_end);
    HeapRegionRemSet::shrink_heap(n_regions());
    g1_policy()->record_new_heap_size(n_regions());
  } else {
    ergo_verbose0(ErgoHeapSizing,
                  "did not shrink the heap",
                  ergo_format_reason("heap shrinking operation failed"));
  }
}

void G1CollectedHeap::shrink(size_t shrink_bytes) {
  verify_region_sets_optional();

  // We should only reach here at the end of a Full GC which means we
  // should not not be holding to any GC alloc regions. The method
  // below will make sure of that and do any remaining clean up.
  abandon_gc_alloc_regions();

  // Instead of tearing down / rebuilding the free lists here, we
  // could instead use the remove_all_pending() method on free_list to
  // remove only the ones that we need to remove.
  tear_down_region_lists();  // We will rebuild them in a moment.
  shrink_helper(shrink_bytes);
  rebuild_region_lists();

  _hrs.verify_optional();
  verify_region_sets_optional();
}

// Public methods.

#ifdef _MSC_VER // the use of 'this' below gets a warning, make it go away
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif // _MSC_VER


G1CollectedHeap::G1CollectedHeap(G1CollectorPolicy* policy_) :
  SharedHeap(policy_),
  _g1_policy(policy_),
  _dirty_card_queue_set(false),
  _into_cset_dirty_card_queue_set(false),
  _is_alive_closure_cm(this),
  _is_alive_closure_stw(this),
  _ref_processor_cm(NULL),
  _ref_processor_stw(NULL),
  _process_strong_tasks(new SubTasksDone(G1H_PS_NumElements)),
  _bot_shared(NULL),
  _objs_with_preserved_marks(NULL), _preserved_marks_of_objs(NULL),
  _evac_failure_scan_stack(NULL) ,
  _mark_in_progress(false),
  _cg1r(NULL), _summary_bytes_used(0),
  _g1mm(NULL),
  _refine_cte_cl(NULL),
  _full_collection(false),
  _free_list("Master Free List"),
  _secondary_free_list("Secondary Free List"),
  _humongous_set("Master Humongous Set"),
  _free_regions_coming(false),
  _young_list(new YoungList(this)),
  _gc_time_stamp(0),
  _retained_old_gc_alloc_region(NULL),
  _surviving_young_words(NULL),
  _full_collections_completed(0),
  _in_cset_fast_test(NULL),
  _in_cset_fast_test_base(NULL),
  _dirty_cards_region_list(NULL) {
  _g1h = this; // To catch bugs.
  if (_process_strong_tasks == NULL || !_process_strong_tasks->valid()) {
    vm_exit_during_initialization("Failed necessary allocation.");
  }

  _humongous_object_threshold_in_words = HeapRegion::GrainWords / 2;

  int n_queues = MAX2((int)ParallelGCThreads, 1);
  _task_queues = new RefToScanQueueSet(n_queues);

  int n_rem_sets = HeapRegionRemSet::num_par_rem_sets();
  assert(n_rem_sets > 0, "Invariant.");

  HeapRegionRemSetIterator** iter_arr =
    NEW_C_HEAP_ARRAY(HeapRegionRemSetIterator*, n_queues);
  for (int i = 0; i < n_queues; i++) {
    iter_arr[i] = new HeapRegionRemSetIterator();
  }
  _rem_set_iterator = iter_arr;

  for (int i = 0; i < n_queues; i++) {
    RefToScanQueue* q = new RefToScanQueue();
    q->initialize();
    _task_queues->register_queue(i, q);
  }

  guarantee(_task_queues != NULL, "task_queues allocation failure.");
}

jint G1CollectedHeap::initialize() {
  CollectedHeap::pre_initialize();
  os::enable_vtime();

  // Necessary to satisfy locking discipline assertions.

  MutexLocker x(Heap_lock);

  // We have to initialize the printer before committing the heap, as
  // it will be used then.
  _hr_printer.set_active(G1PrintHeapRegions);

  // While there are no constraints in the GC code that HeapWordSize
  // be any particular value, there are multiple other areas in the
  // system which believe this to be true (e.g. oop->object_size in some
  // cases incorrectly returns the size in wordSize units rather than
  // HeapWordSize).
  guarantee(HeapWordSize == wordSize, "HeapWordSize must equal wordSize");

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t max_byte_size = collector_policy()->max_heap_byte_size();

  // Ensure that the sizes are properly aligned.
  Universe::check_alignment(init_byte_size, HeapRegion::GrainBytes, "g1 heap");
  Universe::check_alignment(max_byte_size, HeapRegion::GrainBytes, "g1 heap");

  _cg1r = new ConcurrentG1Refine();

  // Reserve the maximum.
  PermanentGenerationSpec* pgs = collector_policy()->permanent_generation();
  // Includes the perm-gen.

  // When compressed oops are enabled, the preferred heap base
  // is calculated by subtracting the requested size from the
  // 32Gb boundary and using the result as the base address for
  // heap reservation. If the requested size is not aligned to
  // HeapRegion::GrainBytes (i.e. the alignment that is passed
  // into the ReservedHeapSpace constructor) then the actual
  // base of the reserved heap may end up differing from the
  // address that was requested (i.e. the preferred heap base).
  // If this happens then we could end up using a non-optimal
  // compressed oops mode.

  // Since max_byte_size is aligned to the size of a heap region (checked
  // above), we also need to align the perm gen size as it might not be.
  const size_t total_reserved = max_byte_size +
                                align_size_up(pgs->max_size(), HeapRegion::GrainBytes);
  Universe::check_alignment(total_reserved, HeapRegion::GrainBytes, "g1 heap and perm");

  char* addr = Universe::preferred_heap_base(total_reserved, Universe::UnscaledNarrowOop);

  ReservedHeapSpace heap_rs(total_reserved, HeapRegion::GrainBytes,
                            UseLargePages, addr);

  if (UseCompressedOops) {
    if (addr != NULL && !heap_rs.is_reserved()) {
      // Failed to reserve at specified address - the requested memory
      // region is taken already, for example, by 'java' launcher.
      // Try again to reserver heap higher.
      addr = Universe::preferred_heap_base(total_reserved, Universe::ZeroBasedNarrowOop);

      ReservedHeapSpace heap_rs0(total_reserved, HeapRegion::GrainBytes,
                                 UseLargePages, addr);

      if (addr != NULL && !heap_rs0.is_reserved()) {
        // Failed to reserve at specified address again - give up.
        addr = Universe::preferred_heap_base(total_reserved, Universe::HeapBasedNarrowOop);
        assert(addr == NULL, "");

        ReservedHeapSpace heap_rs1(total_reserved, HeapRegion::GrainBytes,
                                   UseLargePages, addr);
        heap_rs = heap_rs1;
      } else {
        heap_rs = heap_rs0;
      }
    }
  }

  if (!heap_rs.is_reserved()) {
    vm_exit_during_initialization("Could not reserve enough space for object heap");
    return JNI_ENOMEM;
  }

  // It is important to do this in a way such that concurrent readers can't
  // temporarily think somethings in the heap.  (I've actually seen this
  // happen in asserts: DLD.)
  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*)(heap_rs.base() + heap_rs.size()));

  _expansion_regions = max_byte_size/HeapRegion::GrainBytes;

  // Create the gen rem set (and barrier set) for the entire reserved region.
  _rem_set = collector_policy()->create_rem_set(_reserved, 2);
  set_barrier_set(rem_set()->bs());
  if (barrier_set()->is_a(BarrierSet::ModRef)) {
    _mr_bs = (ModRefBarrierSet*)_barrier_set;
  } else {
    vm_exit_during_initialization("G1 requires a mod ref bs.");
    return JNI_ENOMEM;
  }

  // Also create a G1 rem set.
  if (mr_bs()->is_a(BarrierSet::CardTableModRef)) {
    _g1_rem_set = new G1RemSet(this, (CardTableModRefBS*)mr_bs());
  } else {
    vm_exit_during_initialization("G1 requires a cardtable mod ref bs.");
    return JNI_ENOMEM;
  }

  // Carve out the G1 part of the heap.

  ReservedSpace g1_rs   = heap_rs.first_part(max_byte_size);
  _g1_reserved = MemRegion((HeapWord*)g1_rs.base(),
                           g1_rs.size()/HeapWordSize);
  ReservedSpace perm_gen_rs = heap_rs.last_part(max_byte_size);

  _perm_gen = pgs->init(perm_gen_rs, pgs->init_size(), rem_set());

  _g1_storage.initialize(g1_rs, 0);
  _g1_committed = MemRegion((HeapWord*)_g1_storage.low(), (size_t) 0);
  _hrs.initialize((HeapWord*) _g1_reserved.start(),
                  (HeapWord*) _g1_reserved.end(),
                  _expansion_regions);

  // 6843694 - ensure that the maximum region index can fit
  // in the remembered set structures.
  const size_t max_region_idx = ((size_t)1 << (sizeof(RegionIdx_t)*BitsPerByte-1)) - 1;
  guarantee((max_regions() - 1) <= max_region_idx, "too many regions");

  size_t max_cards_per_region = ((size_t)1 << (sizeof(CardIdx_t)*BitsPerByte-1)) - 1;
  guarantee(HeapRegion::CardsPerRegion > 0, "make sure it's initialized");
  guarantee(HeapRegion::CardsPerRegion < max_cards_per_region,
            "too many cards per region");

  HeapRegionSet::set_unrealistically_long_length(max_regions() + 1);

  _bot_shared = new G1BlockOffsetSharedArray(_reserved,
                                             heap_word_size(init_byte_size));

  _g1h = this;

   _in_cset_fast_test_length = max_regions();
   _in_cset_fast_test_base = NEW_C_HEAP_ARRAY(bool, _in_cset_fast_test_length);

   // We're biasing _in_cset_fast_test to avoid subtracting the
   // beginning of the heap every time we want to index; basically
   // it's the same with what we do with the card table.
   _in_cset_fast_test = _in_cset_fast_test_base -
                ((size_t) _g1_reserved.start() >> HeapRegion::LogOfHRGrainBytes);

   // Clear the _cset_fast_test bitmap in anticipation of adding
   // regions to the incremental collection set for the first
   // evacuation pause.
   clear_cset_fast_test();

  // Create the ConcurrentMark data structure and thread.
  // (Must do this late, so that "max_regions" is defined.)
  _cm       = new ConcurrentMark(heap_rs, (int) max_regions());
  _cmThread = _cm->cmThread();

  // Initialize the from_card cache structure of HeapRegionRemSet.
  HeapRegionRemSet::init_heap(max_regions());

  // Now expand into the initial heap size.
  if (!expand(init_byte_size)) {
    vm_exit_during_initialization("Failed to allocate initial heap.");
    return JNI_ENOMEM;
  }

  // Perform any initialization actions delegated to the policy.
  g1_policy()->init();

  _refine_cte_cl =
    new RefineCardTableEntryClosure(ConcurrentG1RefineThread::sts(),
                                    g1_rem_set(),
                                    concurrent_g1_refine());
  JavaThread::dirty_card_queue_set().set_closure(_refine_cte_cl);

  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               G1SATBProcessCompletedThreshold,
                                               Shared_SATB_Q_lock);

  JavaThread::dirty_card_queue_set().initialize(DirtyCardQ_CBL_mon,
                                                DirtyCardQ_FL_lock,
                                                concurrent_g1_refine()->yellow_zone(),
                                                concurrent_g1_refine()->red_zone(),
                                                Shared_DirtyCardQ_lock);

  if (G1DeferredRSUpdate) {
    dirty_card_queue_set().initialize(DirtyCardQ_CBL_mon,
                                      DirtyCardQ_FL_lock,
                                      -1, // never trigger processing
                                      -1, // no limit on length
                                      Shared_DirtyCardQ_lock,
                                      &JavaThread::dirty_card_queue_set());
  }

  // Initialize the card queue set used to hold cards containing
  // references into the collection set.
  _into_cset_dirty_card_queue_set.initialize(DirtyCardQ_CBL_mon,
                                             DirtyCardQ_FL_lock,
                                             -1, // never trigger processing
                                             -1, // no limit on length
                                             Shared_DirtyCardQ_lock,
                                             &JavaThread::dirty_card_queue_set());

  // In case we're keeping closure specialization stats, initialize those
  // counts and that mechanism.
  SpecializationStats::clear();

  // Do later initialization work for concurrent refinement.
  _cg1r->init();

  // Here we allocate the dummy full region that is required by the
  // G1AllocRegion class. If we don't pass an address in the reserved
  // space here, lots of asserts fire.

  HeapRegion* dummy_region = new_heap_region(0 /* index of bottom region */,
                                             _g1_reserved.start());
  // We'll re-use the same region whether the alloc region will
  // require BOT updates or not and, if it doesn't, then a non-young
  // region will complain that it cannot support allocations without
  // BOT updates. So we'll tag the dummy region as young to avoid that.
  dummy_region->set_young();
  // Make sure it's full.
  dummy_region->set_top(dummy_region->end());
  G1AllocRegion::setup(this, dummy_region);

  init_mutator_alloc_region();

  // Do create of the monitoring and management support so that
  // values in the heap have been properly initialized.
  _g1mm = new G1MonitoringSupport(this);

  return JNI_OK;
}

void G1CollectedHeap::ref_processing_init() {
  // Reference processing in G1 currently works as follows:
  //
  // * There are two reference processor instances. One is
  //   used to record and process discovered references
  //   during concurrent marking; the other is used to
  //   record and process references during STW pauses
  //   (both full and incremental).
  // * Both ref processors need to 'span' the entire heap as
  //   the regions in the collection set may be dotted around.
  //
  // * For the concurrent marking ref processor:
  //   * Reference discovery is enabled at initial marking.
  //   * Reference discovery is disabled and the discovered
  //     references processed etc during remarking.
  //   * Reference discovery is MT (see below).
  //   * Reference discovery requires a barrier (see below).
  //   * Reference processing may or may not be MT
  //     (depending on the value of ParallelRefProcEnabled
  //     and ParallelGCThreads).
  //   * A full GC disables reference discovery by the CM
  //     ref processor and abandons any entries on it's
  //     discovered lists.
  //
  // * For the STW processor:
  //   * Non MT discovery is enabled at the start of a full GC.
  //   * Processing and enqueueing during a full GC is non-MT.
  //   * During a full GC, references are processed after marking.
  //
  //   * Discovery (may or may not be MT) is enabled at the start
  //     of an incremental evacuation pause.
  //   * References are processed near the end of a STW evacuation pause.
  //   * For both types of GC:
  //     * Discovery is atomic - i.e. not concurrent.
  //     * Reference discovery will not need a barrier.

  SharedHeap::ref_processing_init();
  MemRegion mr = reserved_region();

  // Concurrent Mark ref processor
  _ref_processor_cm =
    new ReferenceProcessor(mr,    // span
                           ParallelRefProcEnabled && (ParallelGCThreads > 1),
                                // mt processing
                           (int) ParallelGCThreads,
                                // degree of mt processing
                           (ParallelGCThreads > 1) || (ConcGCThreads > 1),
                                // mt discovery
                           (int) MAX2(ParallelGCThreads, ConcGCThreads),
                                // degree of mt discovery
                           false,
                                // Reference discovery is not atomic
                           &_is_alive_closure_cm,
                                // is alive closure
                                // (for efficiency/performance)
                           true);
                                // Setting next fields of discovered
                                // lists requires a barrier.

  // STW ref processor
  _ref_processor_stw =
    new ReferenceProcessor(mr,    // span
                           ParallelRefProcEnabled && (ParallelGCThreads > 1),
                                // mt processing
                           MAX2((int)ParallelGCThreads, 1),
                                // degree of mt processing
                           (ParallelGCThreads > 1),
                                // mt discovery
                           MAX2((int)ParallelGCThreads, 1),
                                // degree of mt discovery
                           true,
                                // Reference discovery is atomic
                           &_is_alive_closure_stw,
                                // is alive closure
                                // (for efficiency/performance)
                           false);
                                // Setting next fields of discovered
                                // lists requires a barrier.
}

size_t G1CollectedHeap::capacity() const {
  return _g1_committed.byte_size();
}

void G1CollectedHeap::iterate_dirty_card_closure(CardTableEntryClosure* cl,
                                                 DirtyCardQueue* into_cset_dcq,
                                                 bool concurrent,
                                                 int worker_i) {
  // Clean cards in the hot card cache
  concurrent_g1_refine()->clean_up_cache(worker_i, g1_rem_set(), into_cset_dcq);

  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  int n_completed_buffers = 0;
  while (dcqs.apply_closure_to_completed_buffer(cl, worker_i, 0, true)) {
    n_completed_buffers++;
  }
  g1_policy()->record_update_rs_processed_buffers(worker_i,
                                                  (double) n_completed_buffers);
  dcqs.clear_n_completed_buffers();
  assert(!dcqs.completed_buffers_exist_dirty(), "Completed buffers exist!");
}


// Computes the sum of the storage used by the various regions.

size_t G1CollectedHeap::used() const {
  assert(Heap_lock->owner() != NULL,
         "Should be owned on this thread's behalf.");
  size_t result = _summary_bytes_used;
  // Read only once in case it is set to NULL concurrently
  HeapRegion* hr = _mutator_alloc_region.get();
  if (hr != NULL)
    result += hr->used();
  return result;
}

size_t G1CollectedHeap::used_unlocked() const {
  size_t result = _summary_bytes_used;
  return result;
}

class SumUsedClosure: public HeapRegionClosure {
  size_t _used;
public:
  SumUsedClosure() : _used(0) {}
  bool doHeapRegion(HeapRegion* r) {
    if (!r->continuesHumongous()) {
      _used += r->used();
    }
    return false;
  }
  size_t result() { return _used; }
};

size_t G1CollectedHeap::recalculate_used() const {
  SumUsedClosure blk;
  heap_region_iterate(&blk);
  return blk.result();
}

size_t G1CollectedHeap::unsafe_max_alloc() {
  if (free_regions() > 0) return HeapRegion::GrainBytes;
  // otherwise, is there space in the current allocation region?

  // We need to store the current allocation region in a local variable
  // here. The problem is that this method doesn't take any locks and
  // there may be other threads which overwrite the current allocation
  // region field. attempt_allocation(), for example, sets it to NULL
  // and this can happen *after* the NULL check here but before the call
  // to free(), resulting in a SIGSEGV. Note that this doesn't appear
  // to be a problem in the optimized build, since the two loads of the
  // current allocation region field are optimized away.
  HeapRegion* hr = _mutator_alloc_region.get();
  if (hr == NULL) {
    return 0;
  }
  return hr->free();
}

bool G1CollectedHeap::should_do_concurrent_full_gc(GCCause::Cause cause) {
  return
    ((cause == GCCause::_gc_locker           && GCLockerInvokesConcurrent) ||
     (cause == GCCause::_java_lang_system_gc && ExplicitGCInvokesConcurrent));
}

#ifndef PRODUCT
void G1CollectedHeap::allocate_dummy_regions() {
  // Let's fill up most of the region
  size_t word_size = HeapRegion::GrainWords - 1024;
  // And as a result the region we'll allocate will be humongous.
  guarantee(isHumongous(word_size), "sanity");

  for (uintx i = 0; i < G1DummyRegionsPerGC; ++i) {
    // Let's use the existing mechanism for the allocation
    HeapWord* dummy_obj = humongous_obj_allocate(word_size);
    if (dummy_obj != NULL) {
      MemRegion mr(dummy_obj, word_size);
      CollectedHeap::fill_with_object(mr);
    } else {
      // If we can't allocate once, we probably cannot allocate
      // again. Let's get out of the loop.
      break;
    }
  }
}
#endif // !PRODUCT

void G1CollectedHeap::increment_full_collections_completed(bool concurrent) {
  MonitorLockerEx x(FullGCCount_lock, Mutex::_no_safepoint_check_flag);

  // We assume that if concurrent == true, then the caller is a
  // concurrent thread that was joined the Suspendible Thread
  // Set. If there's ever a cheap way to check this, we should add an
  // assert here.

  // We have already incremented _total_full_collections at the start
  // of the GC, so total_full_collections() represents how many full
  // collections have been started.
  unsigned int full_collections_started = total_full_collections();

  // Given that this method is called at the end of a Full GC or of a
  // concurrent cycle, and those can be nested (i.e., a Full GC can
  // interrupt a concurrent cycle), the number of full collections
  // completed should be either one (in the case where there was no
  // nesting) or two (when a Full GC interrupted a concurrent cycle)
  // behind the number of full collections started.

  // This is the case for the inner caller, i.e. a Full GC.
  assert(concurrent ||
         (full_collections_started == _full_collections_completed + 1) ||
         (full_collections_started == _full_collections_completed + 2),
         err_msg("for inner caller (Full GC): full_collections_started = %u "
                 "is inconsistent with _full_collections_completed = %u",
                 full_collections_started, _full_collections_completed));

  // This is the case for the outer caller, i.e. the concurrent cycle.
  assert(!concurrent ||
         (full_collections_started == _full_collections_completed + 1),
         err_msg("for outer caller (concurrent cycle): "
                 "full_collections_started = %u "
                 "is inconsistent with _full_collections_completed = %u",
                 full_collections_started, _full_collections_completed));

  _full_collections_completed += 1;

  // We need to clear the "in_progress" flag in the CM thread before
  // we wake up any waiters (especially when ExplicitInvokesConcurrent
  // is set) so that if a waiter requests another System.gc() it doesn't
  // incorrectly see that a marking cyle is still in progress.
  if (concurrent) {
    _cmThread->clear_in_progress();
  }

  // This notify_all() will ensure that a thread that called
  // System.gc() with (with ExplicitGCInvokesConcurrent set or not)
  // and it's waiting for a full GC to finish will be woken up. It is
  // waiting in VM_G1IncCollectionPause::doit_epilogue().
  FullGCCount_lock->notify_all();
}

void G1CollectedHeap::collect_as_vm_thread(GCCause::Cause cause) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  GCCauseSetter gcs(this, cause);
  switch (cause) {
    case GCCause::_heap_inspection:
    case GCCause::_heap_dump: {
      HandleMark hm;
      do_full_collection(false);         // don't clear all soft refs
      break;
    }
    default: // XXX FIX ME
      ShouldNotReachHere(); // Unexpected use of this function
  }
}

void G1CollectedHeap::collect(GCCause::Cause cause) {
  // The caller doesn't have the Heap_lock
  assert(!Heap_lock->owned_by_self(), "this thread should not own the Heap_lock");

  unsigned int gc_count_before;
  unsigned int full_gc_count_before;
  {
    MutexLocker ml(Heap_lock);

    // Read the GC count while holding the Heap_lock
    gc_count_before = SharedHeap::heap()->total_collections();
    full_gc_count_before = SharedHeap::heap()->total_full_collections();
  }

  if (should_do_concurrent_full_gc(cause)) {
    // Schedule an initial-mark evacuation pause that will start a
    // concurrent cycle. We're setting word_size to 0 which means that
    // we are not requesting a post-GC allocation.
    VM_G1IncCollectionPause op(gc_count_before,
                               0,     /* word_size */
                               true,  /* should_initiate_conc_mark */
                               g1_policy()->max_pause_time_ms(),
                               cause);
    VMThread::execute(&op);
  } else {
    if (cause == GCCause::_gc_locker
        DEBUG_ONLY(|| cause == GCCause::_scavenge_alot)) {

      // Schedule a standard evacuation pause. We're setting word_size
      // to 0 which means that we are not requesting a post-GC allocation.
      VM_G1IncCollectionPause op(gc_count_before,
                                 0,     /* word_size */
                                 false, /* should_initiate_conc_mark */
                                 g1_policy()->max_pause_time_ms(),
                                 cause);
      VMThread::execute(&op);
    } else {
      // Schedule a Full GC.
      VM_G1CollectFull op(gc_count_before, full_gc_count_before, cause);
      VMThread::execute(&op);
    }
  }
}

bool G1CollectedHeap::is_in(const void* p) const {
  HeapRegion* hr = _hrs.addr_to_region((HeapWord*) p);
  if (hr != NULL) {
    return hr->is_in(p);
  } else {
    return _perm_gen->as_gen()->is_in(p);
  }
}

// Iteration functions.

// Iterates an OopClosure over all ref-containing fields of objects
// within a HeapRegion.

class IterateOopClosureRegionClosure: public HeapRegionClosure {
  MemRegion _mr;
  OopClosure* _cl;
public:
  IterateOopClosureRegionClosure(MemRegion mr, OopClosure* cl)
    : _mr(mr), _cl(cl) {}
  bool doHeapRegion(HeapRegion* r) {
    if (! r->continuesHumongous()) {
      r->oop_iterate(_cl);
    }
    return false;
  }
};

void G1CollectedHeap::oop_iterate(OopClosure* cl, bool do_perm) {
  IterateOopClosureRegionClosure blk(_g1_committed, cl);
  heap_region_iterate(&blk);
  if (do_perm) {
    perm_gen()->oop_iterate(cl);
  }
}

void G1CollectedHeap::oop_iterate(MemRegion mr, OopClosure* cl, bool do_perm) {
  IterateOopClosureRegionClosure blk(mr, cl);
  heap_region_iterate(&blk);
  if (do_perm) {
    perm_gen()->oop_iterate(cl);
  }
}

// Iterates an ObjectClosure over all objects within a HeapRegion.

class IterateObjectClosureRegionClosure: public HeapRegionClosure {
  ObjectClosure* _cl;
public:
  IterateObjectClosureRegionClosure(ObjectClosure* cl) : _cl(cl) {}
  bool doHeapRegion(HeapRegion* r) {
    if (! r->continuesHumongous()) {
      r->object_iterate(_cl);
    }
    return false;
  }
};

void G1CollectedHeap::object_iterate(ObjectClosure* cl, bool do_perm) {
  IterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
  if (do_perm) {
    perm_gen()->object_iterate(cl);
  }
}

void G1CollectedHeap::object_iterate_since_last_GC(ObjectClosure* cl) {
  // FIXME: is this right?
  guarantee(false, "object_iterate_since_last_GC not supported by G1 heap");
}

// Calls a SpaceClosure on a HeapRegion.

class SpaceClosureRegionClosure: public HeapRegionClosure {
  SpaceClosure* _cl;
public:
  SpaceClosureRegionClosure(SpaceClosure* cl) : _cl(cl) {}
  bool doHeapRegion(HeapRegion* r) {
    _cl->do_space(r);
    return false;
  }
};

void G1CollectedHeap::space_iterate(SpaceClosure* cl) {
  SpaceClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

void G1CollectedHeap::heap_region_iterate(HeapRegionClosure* cl) const {
  _hrs.iterate(cl);
}

void G1CollectedHeap::heap_region_iterate_from(HeapRegion* r,
                                               HeapRegionClosure* cl) const {
  _hrs.iterate_from(r, cl);
}

void
G1CollectedHeap::heap_region_par_iterate_chunked(HeapRegionClosure* cl,
                                                 int worker,
                                                 jint claim_value) {
  const size_t regions = n_regions();
  const size_t worker_num = (G1CollectedHeap::use_parallel_gc_threads() ? ParallelGCThreads : 1);
  // try to spread out the starting points of the workers
  const size_t start_index = regions / worker_num * (size_t) worker;

  // each worker will actually look at all regions
  for (size_t count = 0; count < regions; ++count) {
    const size_t index = (start_index + count) % regions;
    assert(0 <= index && index < regions, "sanity");
    HeapRegion* r = region_at(index);
    // we'll ignore "continues humongous" regions (we'll process them
    // when we come across their corresponding "start humongous"
    // region) and regions already claimed
    if (r->claim_value() == claim_value || r->continuesHumongous()) {
      continue;
    }
    // OK, try to claim it
    if (r->claimHeapRegion(claim_value)) {
      // success!
      assert(!r->continuesHumongous(), "sanity");
      if (r->startsHumongous()) {
        // If the region is "starts humongous" we'll iterate over its
        // "continues humongous" first; in fact we'll do them
        // first. The order is important. In on case, calling the
        // closure on the "starts humongous" region might de-allocate
        // and clear all its "continues humongous" regions and, as a
        // result, we might end up processing them twice. So, we'll do
        // them first (notice: most closures will ignore them anyway) and
        // then we'll do the "starts humongous" region.
        for (size_t ch_index = index + 1; ch_index < regions; ++ch_index) {
          HeapRegion* chr = region_at(ch_index);

          // if the region has already been claimed or it's not
          // "continues humongous" we're done
          if (chr->claim_value() == claim_value ||
              !chr->continuesHumongous()) {
            break;
          }

          // Noone should have claimed it directly. We can given
          // that we claimed its "starts humongous" region.
          assert(chr->claim_value() != claim_value, "sanity");
          assert(chr->humongous_start_region() == r, "sanity");

          if (chr->claimHeapRegion(claim_value)) {
            // we should always be able to claim it; noone else should
            // be trying to claim this region

            bool res2 = cl->doHeapRegion(chr);
            assert(!res2, "Should not abort");

            // Right now, this holds (i.e., no closure that actually
            // does something with "continues humongous" regions
            // clears them). We might have to weaken it in the future,
            // but let's leave these two asserts here for extra safety.
            assert(chr->continuesHumongous(), "should still be the case");
            assert(chr->humongous_start_region() == r, "sanity");
          } else {
            guarantee(false, "we should not reach here");
          }
        }
      }

      assert(!r->continuesHumongous(), "sanity");
      bool res = cl->doHeapRegion(r);
      assert(!res, "Should not abort");
    }
  }
}

class ResetClaimValuesClosure: public HeapRegionClosure {
public:
  bool doHeapRegion(HeapRegion* r) {
    r->set_claim_value(HeapRegion::InitialClaimValue);
    return false;
  }
};

void
G1CollectedHeap::reset_heap_region_claim_values() {
  ResetClaimValuesClosure blk;
  heap_region_iterate(&blk);
}

#ifdef ASSERT
// This checks whether all regions in the heap have the correct claim
// value. I also piggy-backed on this a check to ensure that the
// humongous_start_region() information on "continues humongous"
// regions is correct.

class CheckClaimValuesClosure : public HeapRegionClosure {
private:
  jint _claim_value;
  size_t _failures;
  HeapRegion* _sh_region;
public:
  CheckClaimValuesClosure(jint claim_value) :
    _claim_value(claim_value), _failures(0), _sh_region(NULL) { }
  bool doHeapRegion(HeapRegion* r) {
    if (r->claim_value() != _claim_value) {
      gclog_or_tty->print_cr("Region ["PTR_FORMAT","PTR_FORMAT"), "
                             "claim value = %d, should be %d",
                             r->bottom(), r->end(), r->claim_value(),
                             _claim_value);
      ++_failures;
    }
    if (!r->isHumongous()) {
      _sh_region = NULL;
    } else if (r->startsHumongous()) {
      _sh_region = r;
    } else if (r->continuesHumongous()) {
      if (r->humongous_start_region() != _sh_region) {
        gclog_or_tty->print_cr("Region ["PTR_FORMAT","PTR_FORMAT"), "
                               "HS = "PTR_FORMAT", should be "PTR_FORMAT,
                               r->bottom(), r->end(),
                               r->humongous_start_region(),
                               _sh_region);
        ++_failures;
      }
    }
    return false;
  }
  size_t failures() {
    return _failures;
  }
};

bool G1CollectedHeap::check_heap_region_claim_values(jint claim_value) {
  CheckClaimValuesClosure cl(claim_value);
  heap_region_iterate(&cl);
  return cl.failures() == 0;
}
#endif // ASSERT

void G1CollectedHeap::collection_set_iterate(HeapRegionClosure* cl) {
  HeapRegion* r = g1_policy()->collection_set();
  while (r != NULL) {
    HeapRegion* next = r->next_in_collection_set();
    if (cl->doHeapRegion(r)) {
      cl->incomplete();
      return;
    }
    r = next;
  }
}

void G1CollectedHeap::collection_set_iterate_from(HeapRegion* r,
                                                  HeapRegionClosure *cl) {
  if (r == NULL) {
    // The CSet is empty so there's nothing to do.
    return;
  }

  assert(r->in_collection_set(),
         "Start region must be a member of the collection set.");
  HeapRegion* cur = r;
  while (cur != NULL) {
    HeapRegion* next = cur->next_in_collection_set();
    if (cl->doHeapRegion(cur) && false) {
      cl->incomplete();
      return;
    }
    cur = next;
  }
  cur = g1_policy()->collection_set();
  while (cur != r) {
    HeapRegion* next = cur->next_in_collection_set();
    if (cl->doHeapRegion(cur) && false) {
      cl->incomplete();
      return;
    }
    cur = next;
  }
}

CompactibleSpace* G1CollectedHeap::first_compactible_space() {
  return n_regions() > 0 ? region_at(0) : NULL;
}


Space* G1CollectedHeap::space_containing(const void* addr) const {
  Space* res = heap_region_containing(addr);
  if (res == NULL)
    res = perm_gen()->space_containing(addr);
  return res;
}

HeapWord* G1CollectedHeap::block_start(const void* addr) const {
  Space* sp = space_containing(addr);
  if (sp != NULL) {
    return sp->block_start(addr);
  }
  return NULL;
}

size_t G1CollectedHeap::block_size(const HeapWord* addr) const {
  Space* sp = space_containing(addr);
  assert(sp != NULL, "block_size of address outside of heap");
  return sp->block_size(addr);
}

bool G1CollectedHeap::block_is_obj(const HeapWord* addr) const {
  Space* sp = space_containing(addr);
  return sp->block_is_obj(addr);
}

bool G1CollectedHeap::supports_tlab_allocation() const {
  return true;
}

size_t G1CollectedHeap::tlab_capacity(Thread* ignored) const {
  return HeapRegion::GrainBytes;
}

size_t G1CollectedHeap::unsafe_max_tlab_alloc(Thread* ignored) const {
  // Return the remaining space in the cur alloc region, but not less than
  // the min TLAB size.

  // Also, this value can be at most the humongous object threshold,
  // since we can't allow tlabs to grow big enough to accomodate
  // humongous objects.

  HeapRegion* hr = _mutator_alloc_region.get();
  size_t max_tlab_size = _humongous_object_threshold_in_words * wordSize;
  if (hr == NULL) {
    return max_tlab_size;
  } else {
    return MIN2(MAX2(hr->free(), (size_t) MinTLABSize), max_tlab_size);
  }
}

size_t G1CollectedHeap::max_capacity() const {
  return _g1_reserved.byte_size();
}

jlong G1CollectedHeap::millis_since_last_gc() {
  // assert(false, "NYI");
  return 0;
}

void G1CollectedHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    ensure_parsability(false);
  }
  g1_rem_set()->prepare_for_verify();
}

class VerifyLivenessOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  VerifyOption _vo;
public:
  VerifyLivenessOopClosure(G1CollectedHeap* g1h, VerifyOption vo):
    _g1h(g1h), _vo(vo)
  { }
  void do_oop(narrowOop *p) { do_oop_work(p); }
  void do_oop(      oop *p) { do_oop_work(p); }

  template <class T> void do_oop_work(T *p) {
    oop obj = oopDesc::load_decode_heap_oop(p);
    guarantee(obj == NULL || !_g1h->is_obj_dead_cond(obj, _vo),
              "Dead object referenced by a not dead object");
  }
};

class VerifyObjsInRegionClosure: public ObjectClosure {
private:
  G1CollectedHeap* _g1h;
  size_t _live_bytes;
  HeapRegion *_hr;
  VerifyOption _vo;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  VerifyObjsInRegionClosure(HeapRegion *hr, VerifyOption vo)
    : _live_bytes(0), _hr(hr), _vo(vo) {
    _g1h = G1CollectedHeap::heap();
  }
  void do_object(oop o) {
    VerifyLivenessOopClosure isLive(_g1h, _vo);
    assert(o != NULL, "Huh?");
    if (!_g1h->is_obj_dead_cond(o, _vo)) {
      // If the object is alive according to the mark word,
      // then verify that the marking information agrees.
      // Note we can't verify the contra-positive of the
      // above: if the object is dead (according to the mark
      // word), it may not be marked, or may have been marked
      // but has since became dead, or may have been allocated
      // since the last marking.
      if (_vo == VerifyOption_G1UseMarkWord) {
        guarantee(!_g1h->is_obj_dead(o), "mark word and concurrent mark mismatch");
      }

      o->oop_iterate(&isLive);
      if (!_hr->obj_allocated_since_prev_marking(o)) {
        size_t obj_size = o->size();    // Make sure we don't overflow
        _live_bytes += (obj_size * HeapWordSize);
      }
    }
  }
  size_t live_bytes() { return _live_bytes; }
};

class PrintObjsInRegionClosure : public ObjectClosure {
  HeapRegion *_hr;
  G1CollectedHeap *_g1;
public:
  PrintObjsInRegionClosure(HeapRegion *hr) : _hr(hr) {
    _g1 = G1CollectedHeap::heap();
  };

  void do_object(oop o) {
    if (o != NULL) {
      HeapWord *start = (HeapWord *) o;
      size_t word_sz = o->size();
      gclog_or_tty->print("\nPrinting obj "PTR_FORMAT" of size " SIZE_FORMAT
                          " isMarkedPrev %d isMarkedNext %d isAllocSince %d\n",
                          (void*) o, word_sz,
                          _g1->isMarkedPrev(o),
                          _g1->isMarkedNext(o),
                          _hr->obj_allocated_since_prev_marking(o));
      HeapWord *end = start + word_sz;
      HeapWord *cur;
      int *val;
      for (cur = start; cur < end; cur++) {
        val = (int *) cur;
        gclog_or_tty->print("\t "PTR_FORMAT":"PTR_FORMAT"\n", val, *val);
      }
    }
  }
};

class VerifyRegionClosure: public HeapRegionClosure {
private:
  bool         _allow_dirty;
  bool         _par;
  VerifyOption _vo;
  bool         _failures;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  VerifyRegionClosure(bool allow_dirty, bool par, VerifyOption vo)
    : _allow_dirty(allow_dirty),
      _par(par),
      _vo(vo),
      _failures(false) {}

  bool failures() {
    return _failures;
  }

  bool doHeapRegion(HeapRegion* r) {
    guarantee(_par || r->claim_value() == HeapRegion::InitialClaimValue,
              "Should be unclaimed at verify points.");
    if (!r->continuesHumongous()) {
      bool failures = false;
      r->verify(_allow_dirty, _vo, &failures);
      if (failures) {
        _failures = true;
      } else {
        VerifyObjsInRegionClosure not_dead_yet_cl(r, _vo);
        r->object_iterate(&not_dead_yet_cl);
        if (r->max_live_bytes() < not_dead_yet_cl.live_bytes()) {
          gclog_or_tty->print_cr("["PTR_FORMAT","PTR_FORMAT"] "
                                 "max_live_bytes "SIZE_FORMAT" "
                                 "< calculated "SIZE_FORMAT,
                                 r->bottom(), r->end(),
                                 r->max_live_bytes(),
                                 not_dead_yet_cl.live_bytes());
          _failures = true;
        }
      }
    }
    return false; // stop the region iteration if we hit a failure
  }
};

class VerifyRootsClosure: public OopsInGenClosure {
private:
  G1CollectedHeap* _g1h;
  VerifyOption     _vo;
  bool             _failures;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  VerifyRootsClosure(VerifyOption vo) :
    _g1h(G1CollectedHeap::heap()),
    _vo(vo),
    _failures(false) { }

  bool failures() { return _failures; }

  template <class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      if (_g1h->is_obj_dead_cond(obj, _vo)) {
        gclog_or_tty->print_cr("Root location "PTR_FORMAT" "
                              "points to dead obj "PTR_FORMAT, p, (void*) obj);
        if (_vo == VerifyOption_G1UseMarkWord) {
          gclog_or_tty->print_cr("  Mark word: "PTR_FORMAT, (void*)(obj->mark()));
        }
        obj->print_on(gclog_or_tty);
        _failures = true;
      }
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }
};

// This is the task used for parallel heap verification.

class G1ParVerifyTask: public AbstractGangTask {
private:
  G1CollectedHeap* _g1h;
  bool             _allow_dirty;
  VerifyOption     _vo;
  bool             _failures;

public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  G1ParVerifyTask(G1CollectedHeap* g1h, bool allow_dirty, VerifyOption vo) :
    AbstractGangTask("Parallel verify task"),
    _g1h(g1h),
    _allow_dirty(allow_dirty),
    _vo(vo),
    _failures(false) { }

  bool failures() {
    return _failures;
  }

  void work(int worker_i) {
    HandleMark hm;
    VerifyRegionClosure blk(_allow_dirty, true, _vo);
    _g1h->heap_region_par_iterate_chunked(&blk, worker_i,
                                          HeapRegion::ParVerifyClaimValue);
    if (blk.failures()) {
      _failures = true;
    }
  }
};

void G1CollectedHeap::verify(bool allow_dirty, bool silent) {
  verify(allow_dirty, silent, VerifyOption_G1UsePrevMarking);
}

void G1CollectedHeap::verify(bool allow_dirty,
                             bool silent,
                             VerifyOption vo) {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    if (!silent) { gclog_or_tty->print("Roots (excluding permgen) "); }
    VerifyRootsClosure rootsCl(vo);
    CodeBlobToOopClosure blobsCl(&rootsCl, /*do_marking=*/ false);

    // We apply the relevant closures to all the oops in the
    // system dictionary, the string table and the code cache.
    const int so = SharedHeap::SO_AllClasses | SharedHeap::SO_Strings | SharedHeap::SO_CodeCache;

    process_strong_roots(true,      // activate StrongRootsScope
                         true,      // we set "collecting perm gen" to true,
                                    // so we don't reset the dirty cards in the perm gen.
                         SharedHeap::ScanningOption(so),  // roots scanning options
                         &rootsCl,
                         &blobsCl,
                         &rootsCl);

    // If we're verifying after the marking phase of a Full GC then we can't
    // treat the perm gen as roots into the G1 heap. Some of the objects in
    // the perm gen may be dead and hence not marked. If one of these dead
    // objects is considered to be a root then we may end up with a false
    // "Root location <x> points to dead ob <y>" failure.
    if (vo != VerifyOption_G1UseMarkWord) {
      // Since we used "collecting_perm_gen" == true above, we will not have
      // checked the refs from perm into the G1-collected heap. We check those
      // references explicitly below. Whether the relevant cards are dirty
      // is checked further below in the rem set verification.
      if (!silent) { gclog_or_tty->print("Permgen roots "); }
      perm_gen()->oop_iterate(&rootsCl);
    }
    bool failures = rootsCl.failures();

    if (vo != VerifyOption_G1UseMarkWord) {
      // If we're verifying during a full GC then the region sets
      // will have been torn down at the start of the GC. Therefore
      // verifying the region sets will fail. So we only verify
      // the region sets when not in a full GC.
      if (!silent) { gclog_or_tty->print("HeapRegionSets "); }
      verify_region_sets();
    }

    if (!silent) { gclog_or_tty->print("HeapRegions "); }
    if (GCParallelVerificationEnabled && ParallelGCThreads > 1) {
      assert(check_heap_region_claim_values(HeapRegion::InitialClaimValue),
             "sanity check");

      G1ParVerifyTask task(this, allow_dirty, vo);
      int n_workers = workers()->total_workers();
      set_par_threads(n_workers);
      workers()->run_task(&task);
      set_par_threads(0);
      if (task.failures()) {
        failures = true;
      }

      assert(check_heap_region_claim_values(HeapRegion::ParVerifyClaimValue),
             "sanity check");

      reset_heap_region_claim_values();

      assert(check_heap_region_claim_values(HeapRegion::InitialClaimValue),
             "sanity check");
    } else {
      VerifyRegionClosure blk(allow_dirty, false, vo);
      heap_region_iterate(&blk);
      if (blk.failures()) {
        failures = true;
      }
    }
    if (!silent) gclog_or_tty->print("RemSet ");
    rem_set()->verify();

    if (failures) {
      gclog_or_tty->print_cr("Heap:");
      print_on(gclog_or_tty, true /* extended */);
      gclog_or_tty->print_cr("");
#ifndef PRODUCT
      if (VerifyDuringGC && G1VerifyDuringGCPrintReachable) {
        concurrent_mark()->print_reachable("at-verification-failure",
                                           vo, false /* all */);
      }
#endif
      gclog_or_tty->flush();
    }
    guarantee(!failures, "there should not have been any failures");
  } else {
    if (!silent) gclog_or_tty->print("(SKIPPING roots, heapRegions, remset) ");
  }
}

class PrintRegionClosure: public HeapRegionClosure {
  outputStream* _st;
public:
  PrintRegionClosure(outputStream* st) : _st(st) {}
  bool doHeapRegion(HeapRegion* r) {
    r->print_on(_st);
    return false;
  }
};

void G1CollectedHeap::print() const { print_on(tty); }

void G1CollectedHeap::print_on(outputStream* st) const {
  print_on(st, PrintHeapAtGCExtended);
}

void G1CollectedHeap::print_on(outputStream* st, bool extended) const {
  st->print(" %-20s", "garbage-first heap");
  st->print(" total " SIZE_FORMAT "K, used " SIZE_FORMAT "K",
            capacity()/K, used_unlocked()/K);
  st->print(" [" INTPTR_FORMAT ", " INTPTR_FORMAT ", " INTPTR_FORMAT ")",
            _g1_storage.low_boundary(),
            _g1_storage.high(),
            _g1_storage.high_boundary());
  st->cr();
  st->print("  region size " SIZE_FORMAT "K, ", HeapRegion::GrainBytes / K);
  size_t young_regions = _young_list->length();
  st->print(SIZE_FORMAT " young (" SIZE_FORMAT "K), ",
            young_regions, young_regions * HeapRegion::GrainBytes / K);
  size_t survivor_regions = g1_policy()->recorded_survivor_regions();
  st->print(SIZE_FORMAT " survivors (" SIZE_FORMAT "K)",
            survivor_regions, survivor_regions * HeapRegion::GrainBytes / K);
  st->cr();
  perm()->as_gen()->print_on(st);
  if (extended) {
    st->cr();
    print_on_extended(st);
  }
}

void G1CollectedHeap::print_on_extended(outputStream* st) const {
  PrintRegionClosure blk(st);
  heap_region_iterate(&blk);
}

void G1CollectedHeap::print_gc_threads_on(outputStream* st) const {
  if (G1CollectedHeap::use_parallel_gc_threads()) {
    workers()->print_worker_threads_on(st);
  }
  _cmThread->print_on(st);
  st->cr();
  _cm->print_worker_threads_on(st);
  _cg1r->print_worker_threads_on(st);
  st->cr();
}

void G1CollectedHeap::gc_threads_do(ThreadClosure* tc) const {
  if (G1CollectedHeap::use_parallel_gc_threads()) {
    workers()->threads_do(tc);
  }
  tc->do_thread(_cmThread);
  _cg1r->threads_do(tc);
}

void G1CollectedHeap::print_tracing_info() const {
  // We'll overload this to mean "trace GC pause statistics."
  if (TraceGen0Time || TraceGen1Time) {
    // The "G1CollectorPolicy" is keeping track of these stats, so delegate
    // to that.
    g1_policy()->print_tracing_info();
  }
  if (G1SummarizeRSetStats) {
    g1_rem_set()->print_summary_info();
  }
  if (G1SummarizeConcMark) {
    concurrent_mark()->print_summary_info();
  }
  g1_policy()->print_yg_surv_rate_info();
  SpecializationStats::print();
}

#ifndef PRODUCT
// Helpful for debugging RSet issues.

class PrintRSetsClosure : public HeapRegionClosure {
private:
  const char* _msg;
  size_t _occupied_sum;

public:
  bool doHeapRegion(HeapRegion* r) {
    HeapRegionRemSet* hrrs = r->rem_set();
    size_t occupied = hrrs->occupied();
    _occupied_sum += occupied;

    gclog_or_tty->print_cr("Printing RSet for region "HR_FORMAT,
                           HR_FORMAT_PARAMS(r));
    if (occupied == 0) {
      gclog_or_tty->print_cr("  RSet is empty");
    } else {
      hrrs->print();
    }
    gclog_or_tty->print_cr("----------");
    return false;
  }

  PrintRSetsClosure(const char* msg) : _msg(msg), _occupied_sum(0) {
    gclog_or_tty->cr();
    gclog_or_tty->print_cr("========================================");
    gclog_or_tty->print_cr(msg);
    gclog_or_tty->cr();
  }

  ~PrintRSetsClosure() {
    gclog_or_tty->print_cr("Occupied Sum: "SIZE_FORMAT, _occupied_sum);
    gclog_or_tty->print_cr("========================================");
    gclog_or_tty->cr();
  }
};

void G1CollectedHeap::print_cset_rsets() {
  PrintRSetsClosure cl("Printing CSet RSets");
  collection_set_iterate(&cl);
}

void G1CollectedHeap::print_all_rsets() {
  PrintRSetsClosure cl("Printing All RSets");;
  heap_region_iterate(&cl);
}
#endif // PRODUCT

G1CollectedHeap* G1CollectedHeap::heap() {
  assert(_sh->kind() == CollectedHeap::G1CollectedHeap,
         "not a garbage-first heap");
  return _g1h;
}

void G1CollectedHeap::gc_prologue(bool full /* Ignored */) {
  // always_do_update_barrier = false;
  assert(InlineCacheBuffer::is_empty(), "should have cleaned up ICBuffer");
  // Call allocation profiler
  AllocationProfiler::iterate_since_last_gc();
  // Fill TLAB's and such
  ensure_parsability(true);
}

void G1CollectedHeap::gc_epilogue(bool full /* Ignored */) {
  // FIXME: what is this about?
  // I'm ignoring the "fill_newgen()" call if "alloc_event_enabled"
  // is set.
  COMPILER2_PRESENT(assert(DerivedPointerTable::is_empty(),
                        "derived pointer present"));
  // always_do_update_barrier = true;

  // We have just completed a GC. Update the soft reference
  // policy with the new heap occupancy
  Universe::update_heap_info_at_gc();
}

HeapWord* G1CollectedHeap::do_collection_pause(size_t word_size,
                                               unsigned int gc_count_before,
                                               bool* succeeded) {
  assert_heap_not_locked_and_not_at_safepoint();
  g1_policy()->record_stop_world_start();
  VM_G1IncCollectionPause op(gc_count_before,
                             word_size,
                             false, /* should_initiate_conc_mark */
                             g1_policy()->max_pause_time_ms(),
                             GCCause::_g1_inc_collection_pause);
  VMThread::execute(&op);

  HeapWord* result = op.result();
  bool ret_succeeded = op.prologue_succeeded() && op.pause_succeeded();
  assert(result == NULL || ret_succeeded,
         "the result should be NULL if the VM did not succeed");
  *succeeded = ret_succeeded;

  assert_heap_not_locked();
  return result;
}

void
G1CollectedHeap::doConcurrentMark() {
  MutexLockerEx x(CGC_lock, Mutex::_no_safepoint_check_flag);
  if (!_cmThread->in_progress()) {
    _cmThread->set_started();
    CGC_lock->notify();
  }
}

// <NEW PREDICTION>

double G1CollectedHeap::predict_region_elapsed_time_ms(HeapRegion *hr,
                                                       bool young) {
  return _g1_policy->predict_region_elapsed_time_ms(hr, young);
}

void G1CollectedHeap::check_if_region_is_too_expensive(double
                                                           predicted_time_ms) {
  _g1_policy->check_if_region_is_too_expensive(predicted_time_ms);
}

size_t G1CollectedHeap::pending_card_num() {
  size_t extra_cards = 0;
  JavaThread *curr = Threads::first();
  while (curr != NULL) {
    DirtyCardQueue& dcq = curr->dirty_card_queue();
    extra_cards += dcq.size();
    curr = curr->next();
  }
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  size_t buffer_size = dcqs.buffer_size();
  size_t buffer_num = dcqs.completed_buffers_num();
  return buffer_size * buffer_num + extra_cards;
}

size_t G1CollectedHeap::max_pending_card_num() {
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  size_t buffer_size = dcqs.buffer_size();
  size_t buffer_num  = dcqs.completed_buffers_num();
  int thread_num  = Threads::number_of_threads();
  return (buffer_num + thread_num) * buffer_size;
}

size_t G1CollectedHeap::cards_scanned() {
  return g1_rem_set()->cardsScanned();
}

void
G1CollectedHeap::setup_surviving_young_words() {
  guarantee( _surviving_young_words == NULL, "pre-condition" );
  size_t array_length = g1_policy()->young_cset_length();
  _surviving_young_words = NEW_C_HEAP_ARRAY(size_t, array_length);
  if (_surviving_young_words == NULL) {
    vm_exit_out_of_memory(sizeof(size_t) * array_length,
                          "Not enough space for young surv words summary.");
  }
  memset(_surviving_young_words, 0, array_length * sizeof(size_t));
#ifdef ASSERT
  for (size_t i = 0;  i < array_length; ++i) {
    assert( _surviving_young_words[i] == 0, "memset above" );
  }
#endif // !ASSERT
}

void
G1CollectedHeap::update_surviving_young_words(size_t* surv_young_words) {
  MutexLockerEx x(ParGCRareEvent_lock, Mutex::_no_safepoint_check_flag);
  size_t array_length = g1_policy()->young_cset_length();
  for (size_t i = 0; i < array_length; ++i)
    _surviving_young_words[i] += surv_young_words[i];
}

void
G1CollectedHeap::cleanup_surviving_young_words() {
  guarantee( _surviving_young_words != NULL, "pre-condition" );
  FREE_C_HEAP_ARRAY(size_t, _surviving_young_words);
  _surviving_young_words = NULL;
}

// </NEW PREDICTION>

#ifdef ASSERT
class VerifyCSetClosure: public HeapRegionClosure {
public:
  bool doHeapRegion(HeapRegion* hr) {
    // Here we check that the CSet region's RSet is ready for parallel
    // iteration. The fields that we'll verify are only manipulated
    // when the region is part of a CSet and is collected. Afterwards,
    // we reset these fields when we clear the region's RSet (when the
    // region is freed) so they are ready when the region is
    // re-allocated. The only exception to this is if there's an
    // evacuation failure and instead of freeing the region we leave
    // it in the heap. In that case, we reset these fields during
    // evacuation failure handling.
    guarantee(hr->rem_set()->verify_ready_for_par_iteration(), "verification");

    // Here's a good place to add any other checks we'd like to
    // perform on CSet regions.
    return false;
  }
};
#endif // ASSERT

#if TASKQUEUE_STATS
void G1CollectedHeap::print_taskqueue_stats_hdr(outputStream* const st) {
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void G1CollectedHeap::print_taskqueue_stats(outputStream* const st) const {
  print_taskqueue_stats_hdr(st);

  TaskQueueStats totals;
  const int n = workers() != NULL ? workers()->total_workers() : 1;
  for (int i = 0; i < n; ++i) {
    st->print("%3d ", i); task_queue(i)->stats.print(st); st->cr();
    totals += task_queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();

  DEBUG_ONLY(totals.verify());
}

void G1CollectedHeap::reset_taskqueue_stats() {
  const int n = workers() != NULL ? workers()->total_workers() : 1;
  for (int i = 0; i < n; ++i) {
    task_queue(i)->stats.reset();
  }
}
#endif // TASKQUEUE_STATS

bool
G1CollectedHeap::do_collection_pause_at_safepoint(double target_pause_time_ms) {
  assert_at_safepoint(true /* should_be_vm_thread */);
  guarantee(!is_gc_active(), "collection is not reentrant");

  if (GC_locker::check_active_before_gc()) {
    return false;
  }

  SvcGCMarker sgcm(SvcGCMarker::MINOR);
  ResourceMark rm;

  if (PrintHeapAtGC) {
    Universe::print_heap_before_gc();
  }

  verify_region_sets_optional();
  verify_dirty_young_regions();

  {
    // This call will decide whether this pause is an initial-mark
    // pause. If it is, during_initial_mark_pause() will return true
    // for the duration of this pause.
    g1_policy()->decide_on_conc_mark_initiation();

    // We do not allow initial-mark to be piggy-backed on a
    // partially-young GC.
    assert(!g1_policy()->during_initial_mark_pause() ||
            g1_policy()->full_young_gcs(), "sanity");

    // We also do not allow partially-young GCs during marking.
    assert(!mark_in_progress() || g1_policy()->full_young_gcs(), "sanity");

    char verbose_str[128];
    sprintf(verbose_str, "GC pause ");
    if (g1_policy()->full_young_gcs()) {
      strcat(verbose_str, "(young)");
    } else {
      strcat(verbose_str, "(partial)");
    }
    if (g1_policy()->during_initial_mark_pause()) {
      strcat(verbose_str, " (initial-mark)");
      // We are about to start a marking cycle, so we increment the
      // full collection counter.
      increment_total_full_collections();
    }

    // if PrintGCDetails is on, we'll print long statistics information
    // in the collector policy code, so let's not print this as the output
    // is messy if we do.
    gclog_or_tty->date_stamp(PrintGC && PrintGCDateStamps);
    TraceCPUTime tcpu(PrintGCDetails, true, gclog_or_tty);
    TraceTime t(verbose_str, PrintGC && !PrintGCDetails, true, gclog_or_tty);

    TraceCollectorStats tcs(g1mm()->incremental_collection_counters());
    TraceMemoryManagerStats tms(false /* fullGC */, gc_cause());

    // If the secondary_free_list is not empty, append it to the
    // free_list. No need to wait for the cleanup operation to finish;
    // the region allocation code will check the secondary_free_list
    // and wait if necessary. If the G1StressConcRegionFreeing flag is
    // set, skip this step so that the region allocation code has to
    // get entries from the secondary_free_list.
    if (!G1StressConcRegionFreeing) {
      append_secondary_free_list_if_not_empty_with_lock();
    }

    assert(check_young_list_well_formed(),
      "young list should be well formed");

    { // Call to jvmpi::post_class_unload_events must occur outside of active GC
      IsGCActiveMark x;

      gc_prologue(false);
      increment_total_collections(false /* full gc */);
      increment_gc_time_stamp();

      if (VerifyBeforeGC && total_collections() >= VerifyGCStartAt) {
        HandleMark hm;  // Discard invalid handles created during verification
        gclog_or_tty->print(" VerifyBeforeGC:");
        prepare_for_verify();
        Universe::verify(/* allow dirty */ false,
                         /* silent      */ false,
                         /* option      */ VerifyOption_G1UsePrevMarking);

      }

      COMPILER2_PRESENT(DerivedPointerTable::clear());

      // Please see comment in g1CollectedHeap.hpp and
      // G1CollectedHeap::ref_processing_init() to see how
      // reference processing currently works in G1.

      // Enable discovery in the STW reference processor
      ref_processor_stw()->enable_discovery(true /*verify_disabled*/,
                                            true /*verify_no_refs*/);

      {
        // We want to temporarily turn off discovery by the
        // CM ref processor, if necessary, and turn it back on
        // on again later if we do. Using a scoped
        // NoRefDiscovery object will do this.
        NoRefDiscovery no_cm_discovery(ref_processor_cm());

        // Forget the current alloc region (we might even choose it to be part
        // of the collection set!).
        release_mutator_alloc_region();

        // We should call this after we retire the mutator alloc
        // region(s) so that all the ALLOC / RETIRE events are generated
        // before the start GC event.
        _hr_printer.start_gc(false /* full */, (size_t) total_collections());

        // The elapsed time induced by the start time below deliberately elides
        // the possible verification above.
        double start_time_sec = os::elapsedTime();
        size_t start_used_bytes = used();

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("\nBefore recording pause start.\nYoung_list:");
        _young_list->print();
        g1_policy()->print_collection_set(g1_policy()->inc_cset_head(), gclog_or_tty);
#endif // YOUNG_LIST_VERBOSE

        g1_policy()->record_collection_pause_start(start_time_sec,
                                                   start_used_bytes);

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("\nAfter recording pause start.\nYoung_list:");
        _young_list->print();
#endif // YOUNG_LIST_VERBOSE

        if (g1_policy()->during_initial_mark_pause()) {
          concurrent_mark()->checkpointRootsInitialPre();
        }
        perm_gen()->save_marks();

        // We must do this before any possible evacuation that should propagate
        // marks.
        if (mark_in_progress()) {
          double start_time_sec = os::elapsedTime();

          _cm->drainAllSATBBuffers();
          double finish_mark_ms = (os::elapsedTime() - start_time_sec) * 1000.0;
          g1_policy()->record_satb_drain_time(finish_mark_ms);
        }
        // Record the number of elements currently on the mark stack, so we
        // only iterate over these.  (Since evacuation may add to the mark
        // stack, doing more exposes race conditions.)  If no mark is in
        // progress, this will be zero.
        _cm->set_oops_do_bound();

        if (mark_in_progress()) {
          concurrent_mark()->newCSet();
        }

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("\nBefore choosing collection set.\nYoung_list:");
        _young_list->print();
        g1_policy()->print_collection_set(g1_policy()->inc_cset_head(), gclog_or_tty);
#endif // YOUNG_LIST_VERBOSE

        g1_policy()->choose_collection_set(target_pause_time_ms);

        if (_hr_printer.is_active()) {
          HeapRegion* hr = g1_policy()->collection_set();
          while (hr != NULL) {
            G1HRPrinter::RegionType type;
            if (!hr->is_young()) {
              type = G1HRPrinter::Old;
            } else if (hr->is_survivor()) {
              type = G1HRPrinter::Survivor;
            } else {
              type = G1HRPrinter::Eden;
            }
            _hr_printer.cset(hr);
            hr = hr->next_in_collection_set();
          }
        }

        // We have chosen the complete collection set. If marking is
        // active then, we clear the region fields of any of the
        // concurrent marking tasks whose region fields point into
        // the collection set as these values will become stale. This
        // will cause the owning marking threads to claim a new region
        // when marking restarts.
        if (mark_in_progress()) {
          concurrent_mark()->reset_active_task_region_fields_in_cset();
        }

#ifdef ASSERT
        VerifyCSetClosure cl;
        collection_set_iterate(&cl);
#endif // ASSERT

        setup_surviving_young_words();

        // Initialize the GC alloc regions.
        init_gc_alloc_regions();

        // Actually do the work...
        evacuate_collection_set();

        free_collection_set(g1_policy()->collection_set());
        g1_policy()->clear_collection_set();

        cleanup_surviving_young_words();

        // Start a new incremental collection set for the next pause.
        g1_policy()->start_incremental_cset_building();

        // Clear the _cset_fast_test bitmap in anticipation of adding
        // regions to the incremental collection set for the next
        // evacuation pause.
        clear_cset_fast_test();

        _young_list->reset_sampled_info();

        // Don't check the whole heap at this point as the
        // GC alloc regions from this pause have been tagged
        // as survivors and moved on to the survivor list.
        // Survivor regions will fail the !is_young() check.
        assert(check_young_list_empty(false /* check_heap */),
          "young list should be empty");

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("Before recording survivors.\nYoung List:");
        _young_list->print();
#endif // YOUNG_LIST_VERBOSE

        g1_policy()->record_survivor_regions(_young_list->survivor_length(),
                                            _young_list->first_survivor_region(),
                                            _young_list->last_survivor_region());

        _young_list->reset_auxilary_lists();

        if (evacuation_failed()) {
          _summary_bytes_used = recalculate_used();
        } else {
          // The "used" of the the collection set have already been subtracted
          // when they were freed.  Add in the bytes evacuated.
          _summary_bytes_used += g1_policy()->bytes_copied_during_gc();
        }

        if (g1_policy()->during_initial_mark_pause()) {
          concurrent_mark()->checkpointRootsInitialPost();
          set_marking_started();
          // CAUTION: after the doConcurrentMark() call below,
          // the concurrent marking thread(s) could be running
          // concurrently with us. Make sure that anything after
          // this point does not assume that we are the only GC thread
          // running. Note: of course, the actual marking work will
          // not start until the safepoint itself is released in
          // ConcurrentGCThread::safepoint_desynchronize().
          doConcurrentMark();
        }

        allocate_dummy_regions();

#if YOUNG_LIST_VERBOSE
        gclog_or_tty->print_cr("\nEnd of the pause.\nYoung_list:");
        _young_list->print();
        g1_policy()->print_collection_set(g1_policy()->inc_cset_head(), gclog_or_tty);
#endif // YOUNG_LIST_VERBOSE

        init_mutator_alloc_region();

        {
          size_t expand_bytes = g1_policy()->expansion_amount();
          if (expand_bytes > 0) {
            size_t bytes_before = capacity();
            if (!expand(expand_bytes)) {
              // We failed to expand the heap so let's verify that
              // committed/uncommitted amount match the backing store
              assert(capacity() == _g1_storage.committed_size(), "committed size mismatch");
              assert(max_capacity() == _g1_storage.reserved_size(), "reserved size mismatch");
            }
          }
        }

        double end_time_sec = os::elapsedTime();
        double pause_time_ms = (end_time_sec - start_time_sec) * MILLIUNITS;
        g1_policy()->record_pause_time_ms(pause_time_ms);
        g1_policy()->record_collection_pause_end();

        MemoryService::track_memory_usage();

        // In prepare_for_verify() below we'll need to scan the deferred
        // update buffers to bring the RSets up-to-date if
        // G1HRRSFlushLogBuffersOnVerify has been set. While scanning
        // the update buffers we'll probably need to scan cards on the
        // regions we just allocated to (i.e., the GC alloc
        // regions). However, during the last GC we called
        // set_saved_mark() on all the GC alloc regions, so card
        // scanning might skip the [saved_mark_word()...top()] area of
        // those regions (i.e., the area we allocated objects into
        // during the last GC). But it shouldn't. Given that
        // saved_mark_word() is conditional on whether the GC time stamp
        // on the region is current or not, by incrementing the GC time
        // stamp here we invalidate all the GC time stamps on all the
        // regions and saved_mark_word() will simply return top() for
        // all the regions. This is a nicer way of ensuring this rather
        // than iterating over the regions and fixing them. In fact, the
        // GC time stamp increment here also ensures that
        // saved_mark_word() will return top() between pauses, i.e.,
        // during concurrent refinement. So we don't need the
        // is_gc_active() check to decided which top to use when
        // scanning cards (see CR 7039627).
        increment_gc_time_stamp();

        if (VerifyAfterGC && total_collections() >= VerifyGCStartAt) {
          HandleMark hm;  // Discard invalid handles created during verification
          gclog_or_tty->print(" VerifyAfterGC:");
          prepare_for_verify();
          Universe::verify(/* allow dirty */ true,
                           /* silent      */ false,
                           /* option      */ VerifyOption_G1UsePrevMarking);
        }

        assert(!ref_processor_stw()->discovery_enabled(), "Postcondition");
        ref_processor_stw()->verify_no_references_recorded();

        // CM reference discovery will be re-enabled if necessary.
      }

      {
        size_t expand_bytes = g1_policy()->expansion_amount();
        if (expand_bytes > 0) {
          size_t bytes_before = capacity();
          // No need for an ergo verbose message here,
          // expansion_amount() does this when it returns a value > 0.
          if (!expand(expand_bytes)) {
            // We failed to expand the heap so let's verify that
            // committed/uncommitted amount match the backing store
            assert(capacity() == _g1_storage.committed_size(), "committed size mismatch");
            assert(max_capacity() == _g1_storage.reserved_size(), "reserved size mismatch");
          }
        }
      }

      // We should do this after we potentially expand the heap so
      // that all the COMMIT events are generated before the end GC
      // event, and after we retire the GC alloc regions so that all
      // RETIRE events are generated before the end GC event.
      _hr_printer.end_gc(false /* full */, (size_t) total_collections());

      // We have to do this after we decide whether to expand the heap or not.
      g1_policy()->print_heap_transition();

      if (mark_in_progress()) {
        concurrent_mark()->update_g1_committed();
      }

#ifdef TRACESPINNING
      ParallelTaskTerminator::print_termination_counts();
#endif

      gc_epilogue(false);
    }

    if (ExitAfterGCNum > 0 && total_collections() == ExitAfterGCNum) {
      gclog_or_tty->print_cr("Stopping after GC #%d", ExitAfterGCNum);
      print_tracing_info();
      vm_exit(-1);
    }
  }

  _hrs.verify_optional();
  verify_region_sets_optional();

  TASKQUEUE_STATS_ONLY(if (ParallelGCVerbose) print_taskqueue_stats());
  TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());

  if (PrintHeapAtGC) {
    Universe::print_heap_after_gc();
  }
  g1mm()->update_sizes();

  if (G1SummarizeRSetStats &&
      (G1SummarizeRSetStatsPeriod > 0) &&
      (total_collections() % G1SummarizeRSetStatsPeriod == 0)) {
    g1_rem_set()->print_summary_info();
  }

  return true;
}

size_t G1CollectedHeap::desired_plab_sz(GCAllocPurpose purpose)
{
  size_t gclab_word_size;
  switch (purpose) {
    case GCAllocForSurvived:
      gclab_word_size = YoungPLABSize;
      break;
    case GCAllocForTenured:
      gclab_word_size = OldPLABSize;
      break;
    default:
      assert(false, "unknown GCAllocPurpose");
      gclab_word_size = OldPLABSize;
      break;
  }
  return gclab_word_size;
}

void G1CollectedHeap::init_mutator_alloc_region() {
  assert(_mutator_alloc_region.get() == NULL, "pre-condition");
  _mutator_alloc_region.init();
}

void G1CollectedHeap::release_mutator_alloc_region() {
  _mutator_alloc_region.release();
  assert(_mutator_alloc_region.get() == NULL, "post-condition");
}

void G1CollectedHeap::init_gc_alloc_regions() {
  assert_at_safepoint(true /* should_be_vm_thread */);

  _survivor_gc_alloc_region.init();
  _old_gc_alloc_region.init();
  HeapRegion* retained_region = _retained_old_gc_alloc_region;
  _retained_old_gc_alloc_region = NULL;

  // We will discard the current GC alloc region if:
  // a) it's in the collection set (it can happen!),
  // b) it's already full (no point in using it),
  // c) it's empty (this means that it was emptied during
  // a cleanup and it should be on the free list now), or
  // d) it's humongous (this means that it was emptied
  // during a cleanup and was added to the free list, but
  // has been subseqently used to allocate a humongous
  // object that may be less than the region size).
  if (retained_region != NULL &&
      !retained_region->in_collection_set() &&
      !(retained_region->top() == retained_region->end()) &&
      !retained_region->is_empty() &&
      !retained_region->isHumongous()) {
    retained_region->set_saved_mark();
    _old_gc_alloc_region.set(retained_region);
    _hr_printer.reuse(retained_region);
  }
}

void G1CollectedHeap::release_gc_alloc_regions() {
  _survivor_gc_alloc_region.release();
  // If we have an old GC alloc region to release, we'll save it in
  // _retained_old_gc_alloc_region. If we don't
  // _retained_old_gc_alloc_region will become NULL. This is what we
  // want either way so no reason to check explicitly for either
  // condition.
  _retained_old_gc_alloc_region = _old_gc_alloc_region.release();
}

void G1CollectedHeap::abandon_gc_alloc_regions() {
  assert(_survivor_gc_alloc_region.get() == NULL, "pre-condition");
  assert(_old_gc_alloc_region.get() == NULL, "pre-condition");
  _retained_old_gc_alloc_region = NULL;
}

void G1CollectedHeap::init_for_evac_failure(OopsInHeapRegionClosure* cl) {
  _drain_in_progress = false;
  set_evac_failure_closure(cl);
  _evac_failure_scan_stack = new (ResourceObj::C_HEAP) GrowableArray<oop>(40, true);
}

void G1CollectedHeap::finalize_for_evac_failure() {
  assert(_evac_failure_scan_stack != NULL &&
         _evac_failure_scan_stack->length() == 0,
         "Postcondition");
  assert(!_drain_in_progress, "Postcondition");
  delete _evac_failure_scan_stack;
  _evac_failure_scan_stack = NULL;
}

class UpdateRSetDeferred : public OopsInHeapRegionClosure {
private:
  G1CollectedHeap* _g1;
  DirtyCardQueue *_dcq;
  CardTableModRefBS* _ct_bs;

public:
  UpdateRSetDeferred(G1CollectedHeap* g1, DirtyCardQueue* dcq) :
    _g1(g1), _ct_bs((CardTableModRefBS*)_g1->barrier_set()), _dcq(dcq) {}

  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(      oop* p) { do_oop_work(p); }
  template <class T> void do_oop_work(T* p) {
    assert(_from->is_in_reserved(p), "paranoia");
    if (!_from->is_in_reserved(oopDesc::load_decode_heap_oop(p)) &&
        !_from->is_survivor()) {
      size_t card_index = _ct_bs->index_for(p);
      if (_ct_bs->mark_card_deferred(card_index)) {
        _dcq->enqueue((jbyte*)_ct_bs->byte_for_index(card_index));
      }
    }
  }
};

class RemoveSelfPointerClosure: public ObjectClosure {
private:
  G1CollectedHeap* _g1;
  ConcurrentMark* _cm;
  HeapRegion* _hr;
  size_t _prev_marked_bytes;
  size_t _next_marked_bytes;
  OopsInHeapRegionClosure *_cl;
public:
  RemoveSelfPointerClosure(G1CollectedHeap* g1, HeapRegion* hr,
                           OopsInHeapRegionClosure* cl) :
    _g1(g1), _hr(hr), _cm(_g1->concurrent_mark()),  _prev_marked_bytes(0),
    _next_marked_bytes(0), _cl(cl) {}

  size_t prev_marked_bytes() { return _prev_marked_bytes; }
  size_t next_marked_bytes() { return _next_marked_bytes; }

  // <original comment>
  // The original idea here was to coalesce evacuated and dead objects.
  // However that caused complications with the block offset table (BOT).
  // In particular if there were two TLABs, one of them partially refined.
  // |----- TLAB_1--------|----TLAB_2-~~~(partially refined part)~~~|
  // The BOT entries of the unrefined part of TLAB_2 point to the start
  // of TLAB_2. If the last object of the TLAB_1 and the first object
  // of TLAB_2 are coalesced, then the cards of the unrefined part
  // would point into middle of the filler object.
  // The current approach is to not coalesce and leave the BOT contents intact.
  // </original comment>
  //
  // We now reset the BOT when we start the object iteration over the
  // region and refine its entries for every object we come across. So
  // the above comment is not really relevant and we should be able
  // to coalesce dead objects if we want to.
  void do_object(oop obj) {
    HeapWord* obj_addr = (HeapWord*) obj;
    assert(_hr->is_in(obj_addr), "sanity");
    size_t obj_size = obj->size();
    _hr->update_bot_for_object(obj_addr, obj_size);
    if (obj->is_forwarded() && obj->forwardee() == obj) {
      // The object failed to move.
      assert(!_g1->is_obj_dead(obj), "We should not be preserving dead objs.");
      _cm->markPrev(obj);
      assert(_cm->isPrevMarked(obj), "Should be marked!");
      _prev_marked_bytes += (obj_size * HeapWordSize);
      if (_g1->mark_in_progress() && !_g1->is_obj_ill(obj)) {
        _cm->markAndGrayObjectIfNecessary(obj);
      }
      obj->set_mark(markOopDesc::prototype());
      // While we were processing RSet buffers during the
      // collection, we actually didn't scan any cards on the
      // collection set, since we didn't want to update remebered
      // sets with entries that point into the collection set, given
      // that live objects fromthe collection set are about to move
      // and such entries will be stale very soon. This change also
      // dealt with a reliability issue which involved scanning a
      // card in the collection set and coming across an array that
      // was being chunked and looking malformed. The problem is
      // that, if evacuation fails, we might have remembered set
      // entries missing given that we skipped cards on the
      // collection set. So, we'll recreate such entries now.
      obj->oop_iterate(_cl);
      assert(_cm->isPrevMarked(obj), "Should be marked!");
    } else {
      // The object has been either evacuated or is dead. Fill it with a
      // dummy object.
      MemRegion mr((HeapWord*)obj, obj_size);
      CollectedHeap::fill_with_object(mr);
      _cm->clearRangeBothMaps(mr);
    }
  }
};

void G1CollectedHeap::remove_self_forwarding_pointers() {
  UpdateRSetImmediate immediate_update(_g1h->g1_rem_set());
  DirtyCardQueue dcq(&_g1h->dirty_card_queue_set());
  UpdateRSetDeferred deferred_update(_g1h, &dcq);
  OopsInHeapRegionClosure *cl;
  if (G1DeferredRSUpdate) {
    cl = &deferred_update;
  } else {
    cl = &immediate_update;
  }
  HeapRegion* cur = g1_policy()->collection_set();
  while (cur != NULL) {
    assert(g1_policy()->assertMarkedBytesDataOK(), "Should be!");
    assert(!cur->isHumongous(), "sanity");

    if (cur->evacuation_failed()) {
      assert(cur->in_collection_set(), "bad CS");
      RemoveSelfPointerClosure rspc(_g1h, cur, cl);

      // In the common case we make sure that this is done when the
      // region is freed so that it is "ready-to-go" when it's
      // re-allocated. However, when evacuation failure happens, a
      // region will remain in the heap and might ultimately be added
      // to a CSet in the future. So we have to be careful here and
      // make sure the region's RSet is ready for parallel iteration
      // whenever this might be required in the future.
      cur->rem_set()->reset_for_par_iteration();
      cur->reset_bot();
      cl->set_region(cur);
      cur->object_iterate(&rspc);

      // A number of manipulations to make the TAMS be the current top,
      // and the marked bytes be the ones observed in the iteration.
      if (_g1h->concurrent_mark()->at_least_one_mark_complete()) {
        // The comments below are the postconditions achieved by the
        // calls.  Note especially the last such condition, which says that
        // the count of marked bytes has been properly restored.
        cur->note_start_of_marking(false);
        // _next_top_at_mark_start == top, _next_marked_bytes == 0
        cur->add_to_marked_bytes(rspc.prev_marked_bytes());
        // _next_marked_bytes == prev_marked_bytes.
        cur->note_end_of_marking();
        // _prev_top_at_mark_start == top(),
        // _prev_marked_bytes == prev_marked_bytes
      }
      // If there is no mark in progress, we modified the _next variables
      // above needlessly, but harmlessly.
      if (_g1h->mark_in_progress()) {
        cur->note_start_of_marking(false);
        // _next_top_at_mark_start == top, _next_marked_bytes == 0
        // _next_marked_bytes == next_marked_bytes.
      }
    }
    cur = cur->next_in_collection_set();
  }
  assert(g1_policy()->assertMarkedBytesDataOK(), "Should be!");

  // Now restore saved marks, if any.
  if (_objs_with_preserved_marks != NULL) {
    assert(_preserved_marks_of_objs != NULL, "Both or none.");
    guarantee(_objs_with_preserved_marks->length() ==
              _preserved_marks_of_objs->length(), "Both or none.");
    for (int i = 0; i < _objs_with_preserved_marks->length(); i++) {
      oop obj   = _objs_with_preserved_marks->at(i);
      markOop m = _preserved_marks_of_objs->at(i);
      obj->set_mark(m);
    }
    // Delete the preserved marks growable arrays (allocated on the C heap).
    delete _objs_with_preserved_marks;
    delete _preserved_marks_of_objs;
    _objs_with_preserved_marks = NULL;
    _preserved_marks_of_objs = NULL;
  }
}

void G1CollectedHeap::push_on_evac_failure_scan_stack(oop obj) {
  _evac_failure_scan_stack->push(obj);
}

void G1CollectedHeap::drain_evac_failure_scan_stack() {
  assert(_evac_failure_scan_stack != NULL, "precondition");

  while (_evac_failure_scan_stack->length() > 0) {
     oop obj = _evac_failure_scan_stack->pop();
     _evac_failure_closure->set_region(heap_region_containing(obj));
     obj->oop_iterate_backwards(_evac_failure_closure);
  }
}

oop
G1CollectedHeap::handle_evacuation_failure_par(OopsInHeapRegionClosure* cl,
                                               oop old,
                                               bool should_mark_root) {
  assert(obj_in_cs(old),
         err_msg("obj: "PTR_FORMAT" should still be in the CSet",
                 (HeapWord*) old));
  markOop m = old->mark();
  oop forward_ptr = old->forward_to_atomic(old);
  if (forward_ptr == NULL) {
    // Forward-to-self succeeded.

    // should_mark_root will be true when this routine is called
    // from a root scanning closure during an initial mark pause.
    // In this case the thread that succeeds in self-forwarding the
    // object is also responsible for marking the object.
    if (should_mark_root) {
      assert(!oopDesc::is_null(old), "shouldn't be");
      _cm->grayRoot(old);
    }

    if (_evac_failure_closure != cl) {
      MutexLockerEx x(EvacFailureStack_lock, Mutex::_no_safepoint_check_flag);
      assert(!_drain_in_progress,
             "Should only be true while someone holds the lock.");
      // Set the global evac-failure closure to the current thread's.
      assert(_evac_failure_closure == NULL, "Or locking has failed.");
      set_evac_failure_closure(cl);
      // Now do the common part.
      handle_evacuation_failure_common(old, m);
      // Reset to NULL.
      set_evac_failure_closure(NULL);
    } else {
      // The lock is already held, and this is recursive.
      assert(_drain_in_progress, "This should only be the recursive case.");
      handle_evacuation_failure_common(old, m);
    }
    return old;
  } else {
    // Forward-to-self failed. Either someone else managed to allocate
    // space for this object (old != forward_ptr) or they beat us in
    // self-forwarding it (old == forward_ptr).
    assert(old == forward_ptr || !obj_in_cs(forward_ptr),
           err_msg("obj: "PTR_FORMAT" forwarded to: "PTR_FORMAT" "
                   "should not be in the CSet",
                   (HeapWord*) old, (HeapWord*) forward_ptr));
    return forward_ptr;
  }
}

void G1CollectedHeap::handle_evacuation_failure_common(oop old, markOop m) {
  set_evacuation_failed(true);

  preserve_mark_if_necessary(old, m);

  HeapRegion* r = heap_region_containing(old);
  if (!r->evacuation_failed()) {
    r->set_evacuation_failed(true);
    _hr_printer.evac_failure(r);
  }

  push_on_evac_failure_scan_stack(old);

  if (!_drain_in_progress) {
    // prevent recursion in copy_to_survivor_space()
    _drain_in_progress = true;
    drain_evac_failure_scan_stack();
    _drain_in_progress = false;
  }
}

void G1CollectedHeap::preserve_mark_if_necessary(oop obj, markOop m) {
  assert(evacuation_failed(), "Oversaving!");
  // We want to call the "for_promotion_failure" version only in the
  // case of a promotion failure.
  if (m->must_be_preserved_for_promotion_failure(obj)) {
    if (_objs_with_preserved_marks == NULL) {
      assert(_preserved_marks_of_objs == NULL, "Both or none.");
      _objs_with_preserved_marks =
        new (ResourceObj::C_HEAP) GrowableArray<oop>(40, true);
      _preserved_marks_of_objs =
        new (ResourceObj::C_HEAP) GrowableArray<markOop>(40, true);
    }
    _objs_with_preserved_marks->push(obj);
    _preserved_marks_of_objs->push(m);
  }
}

HeapWord* G1CollectedHeap::par_allocate_during_gc(GCAllocPurpose purpose,
                                                  size_t word_size) {
  if (purpose == GCAllocForSurvived) {
    HeapWord* result = survivor_attempt_allocation(word_size);
    if (result != NULL) {
      return result;
    } else {
      // Let's try to allocate in the old gen in case we can fit the
      // object there.
      return old_attempt_allocation(word_size);
    }
  } else {
    assert(purpose ==  GCAllocForTenured, "sanity");
    HeapWord* result = old_attempt_allocation(word_size);
    if (result != NULL) {
      return result;
    } else {
      // Let's try to allocate in the survivors in case we can fit the
      // object there.
      return survivor_attempt_allocation(word_size);
    }
  }

  ShouldNotReachHere();
  // Trying to keep some compilers happy.
  return NULL;
}

#ifndef PRODUCT
bool GCLabBitMapClosure::do_bit(size_t offset) {
  HeapWord* addr = _bitmap->offsetToHeapWord(offset);
  guarantee(_cm->isMarked(oop(addr)), "it should be!");
  return true;
}
#endif // PRODUCT

G1ParGCAllocBuffer::G1ParGCAllocBuffer(size_t gclab_word_size) :
  ParGCAllocBuffer(gclab_word_size),
  _should_mark_objects(false),
  _bitmap(G1CollectedHeap::heap()->reserved_region().start(), gclab_word_size),
  _retired(false)
{
  //_should_mark_objects is set to true when G1ParCopyHelper needs to
  // mark the forwarded location of an evacuated object.
  // We set _should_mark_objects to true if marking is active, i.e. when we
  // need to propagate a mark, or during an initial mark pause, i.e. when we
  // need to mark objects immediately reachable by the roots.
  if (G1CollectedHeap::heap()->mark_in_progress() ||
      G1CollectedHeap::heap()->g1_policy()->during_initial_mark_pause()) {
    _should_mark_objects = true;
  }
}

G1ParScanThreadState::G1ParScanThreadState(G1CollectedHeap* g1h, int queue_num)
  : _g1h(g1h),
    _refs(g1h->task_queue(queue_num)),
    _dcq(&g1h->dirty_card_queue_set()),
    _ct_bs((CardTableModRefBS*)_g1h->barrier_set()),
    _g1_rem(g1h->g1_rem_set()),
    _hash_seed(17), _queue_num(queue_num),
    _term_attempts(0),
    _surviving_alloc_buffer(g1h->desired_plab_sz(GCAllocForSurvived)),
    _tenured_alloc_buffer(g1h->desired_plab_sz(GCAllocForTenured)),
    _age_table(false),
    _strong_roots_time(0), _term_time(0),
    _alloc_buffer_waste(0), _undo_waste(0)
{
  // we allocate G1YoungSurvRateNumRegions plus one entries, since
  // we "sacrifice" entry 0 to keep track of surviving bytes for
  // non-young regions (where the age is -1)
  // We also add a few elements at the beginning and at the end in
  // an attempt to eliminate cache contention
  size_t real_length = 1 + _g1h->g1_policy()->young_cset_length();
  size_t array_length = PADDING_ELEM_NUM +
                        real_length +
                        PADDING_ELEM_NUM;
  _surviving_young_words_base = NEW_C_HEAP_ARRAY(size_t, array_length);
  if (_surviving_young_words_base == NULL)
    vm_exit_out_of_memory(array_length * sizeof(size_t),
                          "Not enough space for young surv histo.");
  _surviving_young_words = _surviving_young_words_base + PADDING_ELEM_NUM;
  memset(_surviving_young_words, 0, real_length * sizeof(size_t));

  _alloc_buffers[GCAllocForSurvived] = &_surviving_alloc_buffer;
  _alloc_buffers[GCAllocForTenured]  = &_tenured_alloc_buffer;

  _start = os::elapsedTime();
}

void
G1ParScanThreadState::print_termination_stats_hdr(outputStream* const st)
{
  st->print_raw_cr("GC Termination Stats");
  st->print_raw_cr("     elapsed  --strong roots-- -------termination-------"
                   " ------waste (KiB)------");
  st->print_raw_cr("thr     ms        ms      %        ms      %    attempts"
                   "  total   alloc    undo");
  st->print_raw_cr("--- --------- --------- ------ --------- ------ --------"
                   " ------- ------- -------");
}

void
G1ParScanThreadState::print_termination_stats(int i,
                                              outputStream* const st) const
{
  const double elapsed_ms = elapsed_time() * 1000.0;
  const double s_roots_ms = strong_roots_time() * 1000.0;
  const double term_ms    = term_time() * 1000.0;
  st->print_cr("%3d %9.2f %9.2f %6.2f "
               "%9.2f %6.2f " SIZE_FORMAT_W(8) " "
               SIZE_FORMAT_W(7) " " SIZE_FORMAT_W(7) " " SIZE_FORMAT_W(7),
               i, elapsed_ms, s_roots_ms, s_roots_ms * 100 / elapsed_ms,
               term_ms, term_ms * 100 / elapsed_ms, term_attempts(),
               (alloc_buffer_waste() + undo_waste()) * HeapWordSize / K,
               alloc_buffer_waste() * HeapWordSize / K,
               undo_waste() * HeapWordSize / K);
}

#ifdef ASSERT
bool G1ParScanThreadState::verify_ref(narrowOop* ref) const {
  assert(ref != NULL, "invariant");
  assert(UseCompressedOops, "sanity");
  assert(!has_partial_array_mask(ref), err_msg("ref=" PTR_FORMAT, ref));
  oop p = oopDesc::load_decode_heap_oop(ref);
  assert(_g1h->is_in_g1_reserved(p),
         err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, ref, intptr_t(p)));
  return true;
}

bool G1ParScanThreadState::verify_ref(oop* ref) const {
  assert(ref != NULL, "invariant");
  if (has_partial_array_mask(ref)) {
    // Must be in the collection set--it's already been copied.
    oop p = clear_partial_array_mask(ref);
    assert(_g1h->obj_in_cs(p),
           err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, ref, intptr_t(p)));
  } else {
    oop p = oopDesc::load_decode_heap_oop(ref);
    assert(_g1h->is_in_g1_reserved(p),
           err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, ref, intptr_t(p)));
  }
  return true;
}

bool G1ParScanThreadState::verify_task(StarTask ref) const {
  if (ref.is_narrow()) {
    return verify_ref((narrowOop*) ref);
  } else {
    return verify_ref((oop*) ref);
  }
}
#endif // ASSERT

void G1ParScanThreadState::trim_queue() {
  assert(_evac_cl != NULL, "not set");
  assert(_evac_failure_cl != NULL, "not set");
  assert(_partial_scan_cl != NULL, "not set");

  StarTask ref;
  do {
    // Drain the overflow stack first, so other threads can steal.
    while (refs()->pop_overflow(ref)) {
      deal_with_reference(ref);
    }

    while (refs()->pop_local(ref)) {
      deal_with_reference(ref);
    }
  } while (!refs()->is_empty());
}

G1ParClosureSuper::G1ParClosureSuper(G1CollectedHeap* g1, G1ParScanThreadState* par_scan_state) :
  _g1(g1), _g1_rem(_g1->g1_rem_set()), _cm(_g1->concurrent_mark()),
  _par_scan_state(par_scan_state),
  _during_initial_mark(_g1->g1_policy()->during_initial_mark_pause()),
  _mark_in_progress(_g1->mark_in_progress()) { }

template <class T> void G1ParCopyHelper::mark_object(T* p) {
  // This is called from do_oop_work for objects that are not
  // in the collection set. Objects in the collection set
  // are marked after they have been evacuated.

  T heap_oop = oopDesc::load_heap_oop(p);
  if (!oopDesc::is_null(heap_oop)) {
    oop obj = oopDesc::decode_heap_oop(heap_oop);
    HeapWord* addr = (HeapWord*)obj;
    if (_g1->is_in_g1_reserved(addr)) {
      _cm->grayRoot(oop(addr));
    }
  }
}

oop G1ParCopyHelper::copy_to_survivor_space(oop old, bool should_mark_root,
                                                     bool should_mark_copy) {
  size_t    word_sz = old->size();
  HeapRegion* from_region = _g1->heap_region_containing_raw(old);
  // +1 to make the -1 indexes valid...
  int       young_index = from_region->young_index_in_cset()+1;
  assert( (from_region->is_young() && young_index > 0) ||
          (!from_region->is_young() && young_index == 0), "invariant" );
  G1CollectorPolicy* g1p = _g1->g1_policy();
  markOop m = old->mark();
  int age = m->has_displaced_mark_helper() ? m->displaced_mark_helper()->age()
                                           : m->age();
  GCAllocPurpose alloc_purpose = g1p->evacuation_destination(from_region, age,
                                                             word_sz);
  HeapWord* obj_ptr = _par_scan_state->allocate(alloc_purpose, word_sz);
  oop       obj     = oop(obj_ptr);

  if (obj_ptr == NULL) {
    // This will either forward-to-self, or detect that someone else has
    // installed a forwarding pointer.
    OopsInHeapRegionClosure* cl = _par_scan_state->evac_failure_closure();
    return _g1->handle_evacuation_failure_par(cl, old, should_mark_root);
  }

  // We're going to allocate linearly, so might as well prefetch ahead.
  Prefetch::write(obj_ptr, PrefetchCopyIntervalInBytes);

  oop forward_ptr = old->forward_to_atomic(obj);
  if (forward_ptr == NULL) {
    Copy::aligned_disjoint_words((HeapWord*) old, obj_ptr, word_sz);
    if (g1p->track_object_age(alloc_purpose)) {
      // We could simply do obj->incr_age(). However, this causes a
      // performance issue. obj->incr_age() will first check whether
      // the object has a displaced mark by checking its mark word;
      // getting the mark word from the new location of the object
      // stalls. So, given that we already have the mark word and we
      // are about to install it anyway, it's better to increase the
      // age on the mark word, when the object does not have a
      // displaced mark word. We're not expecting many objects to have
      // a displaced marked word, so that case is not optimized
      // further (it could be...) and we simply call obj->incr_age().

      if (m->has_displaced_mark_helper()) {
        // in this case, we have to install the mark word first,
        // otherwise obj looks to be forwarded (the old mark word,
        // which contains the forward pointer, was copied)
        obj->set_mark(m);
        obj->incr_age();
      } else {
        m = m->incr_age();
        obj->set_mark(m);
      }
      _par_scan_state->age_table()->add(obj, word_sz);
    } else {
      obj->set_mark(m);
    }

    // Mark the evacuated object or propagate "next" mark bit
    if (should_mark_copy) {
      if (!use_local_bitmaps ||
          !_par_scan_state->alloc_buffer(alloc_purpose)->mark(obj_ptr)) {
        // if we couldn't mark it on the local bitmap (this happens when
        // the object was not allocated in the GCLab), we have to bite
        // the bullet and do the standard parallel mark
        _cm->markAndGrayObjectIfNecessary(obj);
      }

      if (_g1->isMarkedNext(old)) {
        // Unmark the object's old location so that marking
        // doesn't think the old object is alive.
        _cm->nextMarkBitMap()->parClear((HeapWord*)old);
      }
    }

    size_t* surv_young_words = _par_scan_state->surviving_young_words();
    surv_young_words[young_index] += word_sz;

    if (obj->is_objArray() && arrayOop(obj)->length() >= ParGCArrayScanChunk) {
      arrayOop(old)->set_length(0);
      oop* old_p = set_partial_array_mask(old);
      _par_scan_state->push_on_queue(old_p);
    } else {
      // No point in using the slower heap_region_containing() method,
      // given that we know obj is in the heap.
      _scanner->set_region(_g1->heap_region_containing_raw(obj));
      obj->oop_iterate_backwards(_scanner);
    }
  } else {
    _par_scan_state->undo_allocation(alloc_purpose, obj_ptr, word_sz);
    obj = forward_ptr;
  }
  return obj;
}

template <bool do_gen_barrier, G1Barrier barrier, bool do_mark_object>
template <class T>
void G1ParCopyClosure<do_gen_barrier, barrier, do_mark_object>
::do_oop_work(T* p) {
  oop obj = oopDesc::load_decode_heap_oop(p);
  assert(barrier != G1BarrierRS || obj != NULL,
         "Precondition: G1BarrierRS implies obj is nonNull");

  // Marking:
  // If the object is in the collection set, then the thread
  // that copies the object should mark, or propagate the
  // mark to, the evacuated object.
  // If the object is not in the collection set then we
  // should call the mark_object() method depending on the
  // value of the template parameter do_mark_object (which will
  // be true for root scanning closures during an initial mark
  // pause).
  // The mark_object() method first checks whether the object
  // is marked and, if not, attempts to mark the object.

  // here the null check is implicit in the cset_fast_test() test
  if (_g1->in_cset_fast_test(obj)) {
    if (obj->is_forwarded()) {
      oopDesc::encode_store_heap_oop(p, obj->forwardee());
      // If we are a root scanning closure during an initial
      // mark pause (i.e. do_mark_object will be true) then
      // we also need to handle marking of roots in the
      // event of an evacuation failure. In the event of an
      // evacuation failure, the object is forwarded to itself
      // and not copied. For root-scanning closures, the
      // object would be marked after a successful self-forward
      // but an object could be pointed to by both a root and non
      // root location and be self-forwarded by a non-root-scanning
      // closure. Therefore we also have to attempt to mark the
      // self-forwarded root object here.
      if (do_mark_object && obj->forwardee() == obj) {
        mark_object(p);
      }
    } else {
      // During an initial mark pause, objects that are pointed to
      // by the roots need to be marked - even in the event of an
      // evacuation failure. We pass the template parameter
      // do_mark_object (which is true for root scanning closures
      // during an initial mark pause) to copy_to_survivor_space
      // which will pass it on to the evacuation failure handling
      // code. The thread that successfully self-forwards a root
      // object to itself is responsible for marking the object.
      bool should_mark_root = do_mark_object;

      // We need to mark the copied object if we're a root scanning
      // closure during an initial mark pause (i.e. do_mark_object
      // will be true), or the object is already marked and we need
      // to propagate the mark to the evacuated copy.
      bool should_mark_copy = do_mark_object ||
                              _during_initial_mark ||
                              (_mark_in_progress && !_g1->is_obj_ill(obj));

      oop copy_oop = copy_to_survivor_space(obj, should_mark_root,
                                                 should_mark_copy);
      oopDesc::encode_store_heap_oop(p, copy_oop);
    }
    // When scanning the RS, we only care about objs in CS.
    if (barrier == G1BarrierRS) {
      _par_scan_state->update_rs(_from, p, _par_scan_state->queue_num());
    }
  } else {
    // The object is not in collection set. If we're a root scanning
    // closure during an initial mark pause (i.e. do_mark_object will
    // be true) then attempt to mark the object.
    if (do_mark_object) {
      mark_object(p);
    }
  }

  if (barrier == G1BarrierEvac && obj != NULL) {
    _par_scan_state->update_rs(_from, p, _par_scan_state->queue_num());
  }

  if (do_gen_barrier && obj != NULL) {
    par_do_barrier(p);
  }
}

template void G1ParCopyClosure<false, G1BarrierEvac, false>::do_oop_work(oop* p);
template void G1ParCopyClosure<false, G1BarrierEvac, false>::do_oop_work(narrowOop* p);

template <class T> void G1ParScanPartialArrayClosure::do_oop_nv(T* p) {
  assert(has_partial_array_mask(p), "invariant");
  oop old = clear_partial_array_mask(p);
  assert(old->is_objArray(), "must be obj array");
  assert(old->is_forwarded(), "must be forwarded");
  assert(Universe::heap()->is_in_reserved(old), "must be in heap.");

  objArrayOop obj = objArrayOop(old->forwardee());
  assert((void*)old != (void*)old->forwardee(), "self forwarding here?");
  // Process ParGCArrayScanChunk elements now
  // and push the remainder back onto queue
  int start     = arrayOop(old)->length();
  int end       = obj->length();
  int remainder = end - start;
  assert(start <= end, "just checking");
  if (remainder > 2 * ParGCArrayScanChunk) {
    // Test above combines last partial chunk with a full chunk
    end = start + ParGCArrayScanChunk;
    arrayOop(old)->set_length(end);
    // Push remainder.
    oop* old_p = set_partial_array_mask(old);
    assert(arrayOop(old)->length() < obj->length(), "Empty push?");
    _par_scan_state->push_on_queue(old_p);
  } else {
    // Restore length so that the heap remains parsable in
    // case of evacuation failure.
    arrayOop(old)->set_length(end);
  }
  _scanner.set_region(_g1->heap_region_containing_raw(obj));
  // process our set of indices (include header in first chunk)
  obj->oop_iterate_range(&_scanner, start, end);
}

class G1ParEvacuateFollowersClosure : public VoidClosure {
protected:
  G1CollectedHeap*              _g1h;
  G1ParScanThreadState*         _par_scan_state;
  RefToScanQueueSet*            _queues;
  ParallelTaskTerminator*       _terminator;

  G1ParScanThreadState*   par_scan_state() { return _par_scan_state; }
  RefToScanQueueSet*      queues()         { return _queues; }
  ParallelTaskTerminator* terminator()     { return _terminator; }

public:
  G1ParEvacuateFollowersClosure(G1CollectedHeap* g1h,
                                G1ParScanThreadState* par_scan_state,
                                RefToScanQueueSet* queues,
                                ParallelTaskTerminator* terminator)
    : _g1h(g1h), _par_scan_state(par_scan_state),
      _queues(queues), _terminator(terminator) {}

  void do_void();

private:
  inline bool offer_termination();
};

bool G1ParEvacuateFollowersClosure::offer_termination() {
  G1ParScanThreadState* const pss = par_scan_state();
  pss->start_term_time();
  const bool res = terminator()->offer_termination();
  pss->end_term_time();
  return res;
}

void G1ParEvacuateFollowersClosure::do_void() {
  StarTask stolen_task;
  G1ParScanThreadState* const pss = par_scan_state();
  pss->trim_queue();

  do {
    while (queues()->steal(pss->queue_num(), pss->hash_seed(), stolen_task)) {
      assert(pss->verify_task(stolen_task), "sanity");
      if (stolen_task.is_narrow()) {
        pss->deal_with_reference((narrowOop*) stolen_task);
      } else {
        pss->deal_with_reference((oop*) stolen_task);
      }

      // We've just processed a reference and we might have made
      // available new entries on the queues. So we have to make sure
      // we drain the queues as necessary.
      pss->trim_queue();
    }
  } while (!offer_termination());

  pss->retire_alloc_buffers();
}

class G1ParTask : public AbstractGangTask {
protected:
  G1CollectedHeap*       _g1h;
  RefToScanQueueSet      *_queues;
  ParallelTaskTerminator _terminator;
  int _n_workers;

  Mutex _stats_lock;
  Mutex* stats_lock() { return &_stats_lock; }

  size_t getNCards() {
    return (_g1h->capacity() + G1BlockOffsetSharedArray::N_bytes - 1)
      / G1BlockOffsetSharedArray::N_bytes;
  }

public:
  G1ParTask(G1CollectedHeap* g1h, int workers, RefToScanQueueSet *task_queues)
    : AbstractGangTask("G1 collection"),
      _g1h(g1h),
      _queues(task_queues),
      _terminator(workers, _queues),
      _stats_lock(Mutex::leaf, "parallel G1 stats lock", true),
      _n_workers(workers)
  {}

  RefToScanQueueSet* queues() { return _queues; }

  RefToScanQueue *work_queue(int i) {
    return queues()->queue(i);
  }

  void work(int i) {
    if (i >= _n_workers) return;  // no work needed this round

    double start_time_ms = os::elapsedTime() * 1000.0;
    _g1h->g1_policy()->record_gc_worker_start_time(i, start_time_ms);

    ResourceMark rm;
    HandleMark   hm;

    ReferenceProcessor*             rp = _g1h->ref_processor_stw();

    G1ParScanThreadState            pss(_g1h, i);
    G1ParScanHeapEvacClosure        scan_evac_cl(_g1h, &pss, rp);
    G1ParScanHeapEvacFailureClosure evac_failure_cl(_g1h, &pss, rp);
    G1ParScanPartialArrayClosure    partial_scan_cl(_g1h, &pss, rp);

    pss.set_evac_closure(&scan_evac_cl);
    pss.set_evac_failure_closure(&evac_failure_cl);
    pss.set_partial_scan_closure(&partial_scan_cl);

    G1ParScanExtRootClosure        only_scan_root_cl(_g1h, &pss, rp);
    G1ParScanPermClosure           only_scan_perm_cl(_g1h, &pss, rp);

    G1ParScanAndMarkExtRootClosure scan_mark_root_cl(_g1h, &pss, rp);
    G1ParScanAndMarkPermClosure    scan_mark_perm_cl(_g1h, &pss, rp);

    OopClosure*                    scan_root_cl = &only_scan_root_cl;
    OopsInHeapRegionClosure*       scan_perm_cl = &only_scan_perm_cl;

    if (_g1h->g1_policy()->during_initial_mark_pause()) {
      // We also need to mark copied objects.
      scan_root_cl = &scan_mark_root_cl;
      scan_perm_cl = &scan_mark_perm_cl;
    }

    G1ParPushHeapRSClosure          push_heap_rs_cl(_g1h, &pss);

    pss.start_strong_roots();
    _g1h->g1_process_strong_roots(/* not collecting perm */ false,
                                  SharedHeap::SO_AllClasses,
                                  scan_root_cl,
                                  &push_heap_rs_cl,
                                  scan_perm_cl,
                                  i);
    pss.end_strong_roots();

    {
      double start = os::elapsedTime();
      G1ParEvacuateFollowersClosure evac(_g1h, &pss, _queues, &_terminator);
      evac.do_void();
      double elapsed_ms = (os::elapsedTime()-start)*1000.0;
      double term_ms = pss.term_time()*1000.0;
      _g1h->g1_policy()->record_obj_copy_time(i, elapsed_ms-term_ms);
      _g1h->g1_policy()->record_termination(i, term_ms, pss.term_attempts());
    }
    _g1h->g1_policy()->record_thread_age_table(pss.age_table());
    _g1h->update_surviving_young_words(pss.surviving_young_words()+1);

    // Clean up any par-expanded rem sets.
    HeapRegionRemSet::par_cleanup();

    if (ParallelGCVerbose) {
      MutexLocker x(stats_lock());
      pss.print_termination_stats(i);
    }

    assert(pss.refs()->is_empty(), "should be empty");
    double end_time_ms = os::elapsedTime() * 1000.0;
    _g1h->g1_policy()->record_gc_worker_end_time(i, end_time_ms);
  }
};

// *** Common G1 Evacuation Stuff

// This method is run in a GC worker.

void
G1CollectedHeap::
g1_process_strong_roots(bool collecting_perm_gen,
                        SharedHeap::ScanningOption so,
                        OopClosure* scan_non_heap_roots,
                        OopsInHeapRegionClosure* scan_rs,
                        OopsInGenClosure* scan_perm,
                        int worker_i) {

  // First scan the strong roots, including the perm gen.
  double ext_roots_start = os::elapsedTime();
  double closure_app_time_sec = 0.0;

  BufferingOopClosure buf_scan_non_heap_roots(scan_non_heap_roots);
  BufferingOopsInGenClosure buf_scan_perm(scan_perm);
  buf_scan_perm.set_generation(perm_gen());

  // Walk the code cache w/o buffering, because StarTask cannot handle
  // unaligned oop locations.
  CodeBlobToOopClosure eager_scan_code_roots(scan_non_heap_roots, /*do_marking=*/ true);

  process_strong_roots(false, // no scoping; this is parallel code
                       collecting_perm_gen, so,
                       &buf_scan_non_heap_roots,
                       &eager_scan_code_roots,
                       &buf_scan_perm);

  // Now the CM ref_processor roots.
  if (!_process_strong_tasks->is_task_claimed(G1H_PS_refProcessor_oops_do)) {
    // We need to treat the discovered reference lists of the
    // concurrent mark ref processor as roots and keep entries
    // (which are added by the marking threads) on them live
    // until they can be processed at the end of marking.
    ref_processor_cm()->weak_oops_do(&buf_scan_non_heap_roots);
  }

  // Finish up any enqueued closure apps (attributed as object copy time).
  buf_scan_non_heap_roots.done();
  buf_scan_perm.done();

  double ext_roots_end = os::elapsedTime();

  g1_policy()->reset_obj_copy_time(worker_i);
  double obj_copy_time_sec = buf_scan_perm.closure_app_seconds() +
                                buf_scan_non_heap_roots.closure_app_seconds();
  g1_policy()->record_obj_copy_time(worker_i, obj_copy_time_sec * 1000.0);

  double ext_root_time_ms =
    ((ext_roots_end - ext_roots_start) - obj_copy_time_sec) * 1000.0;

  g1_policy()->record_ext_root_scan_time(worker_i, ext_root_time_ms);

  // Scan strong roots in mark stack.
  if (!_process_strong_tasks->is_task_claimed(G1H_PS_mark_stack_oops_do)) {
    concurrent_mark()->oops_do(scan_non_heap_roots);
  }
  double mark_stack_scan_ms = (os::elapsedTime() - ext_roots_end) * 1000.0;
  g1_policy()->record_mark_stack_scan_time(worker_i, mark_stack_scan_ms);

  // Now scan the complement of the collection set.
  if (scan_rs != NULL) {
    g1_rem_set()->oops_into_collection_set_do(scan_rs, worker_i);
  }

  _process_strong_tasks->all_tasks_completed();
}

void
G1CollectedHeap::g1_process_weak_roots(OopClosure* root_closure,
                                       OopClosure* non_root_closure) {
  CodeBlobToOopClosure roots_in_blobs(root_closure, /*do_marking=*/ false);
  SharedHeap::process_weak_roots(root_closure, &roots_in_blobs, non_root_closure);
}

// Weak Reference Processing support

// An always "is_alive" closure that is used to preserve referents.
// If the object is non-null then it's alive.  Used in the preservation
// of referent objects that are pointed to by reference objects
// discovered by the CM ref processor.
class G1AlwaysAliveClosure: public BoolObjectClosure {
  G1CollectedHeap* _g1;
public:
  G1AlwaysAliveClosure(G1CollectedHeap* g1) : _g1(g1) {}
  void do_object(oop p) { assert(false, "Do not call."); }
  bool do_object_b(oop p) {
    if (p != NULL) {
      return true;
    }
    return false;
  }
};

bool G1STWIsAliveClosure::do_object_b(oop p) {
  // An object is reachable if it is outside the collection set,
  // or is inside and copied.
  return !_g1->obj_in_cs(p) || p->is_forwarded();
}

// Non Copying Keep Alive closure
class G1KeepAliveClosure: public OopClosure {
  G1CollectedHeap* _g1;
public:
  G1KeepAliveClosure(G1CollectedHeap* g1) : _g1(g1) {}
  void do_oop(narrowOop* p) { guarantee(false, "Not needed"); }
  void do_oop(      oop* p) {
    oop obj = *p;

    if (_g1->obj_in_cs(obj)) {
      assert( obj->is_forwarded(), "invariant" );
      *p = obj->forwardee();
    }
  }
};

// Copying Keep Alive closure - can be called from both
// serial and parallel code as long as different worker
// threads utilize different G1ParScanThreadState instances
// and different queues.

class G1CopyingKeepAliveClosure: public OopClosure {
  G1CollectedHeap*         _g1h;
  OopClosure*              _copy_non_heap_obj_cl;
  OopsInHeapRegionClosure* _copy_perm_obj_cl;
  G1ParScanThreadState*    _par_scan_state;

public:
  G1CopyingKeepAliveClosure(G1CollectedHeap* g1h,
                            OopClosure* non_heap_obj_cl,
                            OopsInHeapRegionClosure* perm_obj_cl,
                            G1ParScanThreadState* pss):
    _g1h(g1h),
    _copy_non_heap_obj_cl(non_heap_obj_cl),
    _copy_perm_obj_cl(perm_obj_cl),
    _par_scan_state(pss)
  {}

  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(      oop* p) { do_oop_work(p); }

  template <class T> void do_oop_work(T* p) {
    oop obj = oopDesc::load_decode_heap_oop(p);

    if (_g1h->obj_in_cs(obj)) {
      // If the referent object has been forwarded (either copied
      // to a new location or to itself in the event of an
      // evacuation failure) then we need to update the reference
      // field and, if both reference and referent are in the G1
      // heap, update the RSet for the referent.
      //
      // If the referent has not been forwarded then we have to keep
      // it alive by policy. Therefore we have copy the referent.
      //
      // If the reference field is in the G1 heap then we can push
      // on the PSS queue. When the queue is drained (after each
      // phase of reference processing) the object and it's followers
      // will be copied, the reference field set to point to the
      // new location, and the RSet updated. Otherwise we need to
      // use the the non-heap or perm closures directly to copy
      // the refernt object and update the pointer, while avoiding
      // updating the RSet.

      if (_g1h->is_in_g1_reserved(p)) {
        _par_scan_state->push_on_queue(p);
      } else {
        // The reference field is not in the G1 heap.
        if (_g1h->perm_gen()->is_in(p)) {
          _copy_perm_obj_cl->do_oop(p);
        } else {
          _copy_non_heap_obj_cl->do_oop(p);
        }
      }
    }
  }
};

// Serial drain queue closure. Called as the 'complete_gc'
// closure for each discovered list in some of the
// reference processing phases.

class G1STWDrainQueueClosure: public VoidClosure {
protected:
  G1CollectedHeap* _g1h;
  G1ParScanThreadState* _par_scan_state;

  G1ParScanThreadState*   par_scan_state() { return _par_scan_state; }

public:
  G1STWDrainQueueClosure(G1CollectedHeap* g1h, G1ParScanThreadState* pss) :
    _g1h(g1h),
    _par_scan_state(pss)
  { }

  void do_void() {
    G1ParScanThreadState* const pss = par_scan_state();
    pss->trim_queue();
  }
};

// Parallel Reference Processing closures

// Implementation of AbstractRefProcTaskExecutor for parallel reference
// processing during G1 evacuation pauses.

class G1STWRefProcTaskExecutor: public AbstractRefProcTaskExecutor {
private:
  G1CollectedHeap*   _g1h;
  RefToScanQueueSet* _queues;
  WorkGang*          _workers;
  int                _active_workers;

public:
  G1STWRefProcTaskExecutor(G1CollectedHeap* g1h,
                        WorkGang* workers,
                        RefToScanQueueSet *task_queues,
                        int n_workers) :
    _g1h(g1h),
    _queues(task_queues),
    _workers(workers),
    _active_workers(n_workers)
  {
    assert(n_workers > 0, "shouldn't call this otherwise");
  }

  // Executes the given task using concurrent marking worker threads.
  virtual void execute(ProcessTask& task);
  virtual void execute(EnqueueTask& task);
};

// Gang task for possibly parallel reference processing

class G1STWRefProcTaskProxy: public AbstractGangTask {
  typedef AbstractRefProcTaskExecutor::ProcessTask ProcessTask;
  ProcessTask&     _proc_task;
  G1CollectedHeap* _g1h;
  RefToScanQueueSet *_task_queues;
  ParallelTaskTerminator* _terminator;

public:
  G1STWRefProcTaskProxy(ProcessTask& proc_task,
                     G1CollectedHeap* g1h,
                     RefToScanQueueSet *task_queues,
                     ParallelTaskTerminator* terminator) :
    AbstractGangTask("Process reference objects in parallel"),
    _proc_task(proc_task),
    _g1h(g1h),
    _task_queues(task_queues),
    _terminator(terminator)
  {}

  virtual void work(int i) {
    // The reference processing task executed by a single worker.
    ResourceMark rm;
    HandleMark   hm;

    G1STWIsAliveClosure is_alive(_g1h);

    G1ParScanThreadState pss(_g1h, i);

    G1ParScanHeapEvacClosure        scan_evac_cl(_g1h, &pss, NULL);
    G1ParScanHeapEvacFailureClosure evac_failure_cl(_g1h, &pss, NULL);
    G1ParScanPartialArrayClosure    partial_scan_cl(_g1h, &pss, NULL);

    pss.set_evac_closure(&scan_evac_cl);
    pss.set_evac_failure_closure(&evac_failure_cl);
    pss.set_partial_scan_closure(&partial_scan_cl);

    G1ParScanExtRootClosure        only_copy_non_heap_cl(_g1h, &pss, NULL);
    G1ParScanPermClosure           only_copy_perm_cl(_g1h, &pss, NULL);

    G1ParScanAndMarkExtRootClosure copy_mark_non_heap_cl(_g1h, &pss, NULL);
    G1ParScanAndMarkPermClosure    copy_mark_perm_cl(_g1h, &pss, NULL);

    OopClosure*                    copy_non_heap_cl = &only_copy_non_heap_cl;
    OopsInHeapRegionClosure*       copy_perm_cl = &only_copy_perm_cl;

    if (_g1h->g1_policy()->during_initial_mark_pause()) {
      // We also need to mark copied objects.
      copy_non_heap_cl = &copy_mark_non_heap_cl;
      copy_perm_cl = &copy_mark_perm_cl;
    }

    // Keep alive closure.
    G1CopyingKeepAliveClosure keep_alive(_g1h, copy_non_heap_cl, copy_perm_cl, &pss);

    // Complete GC closure
    G1ParEvacuateFollowersClosure drain_queue(_g1h, &pss, _task_queues, _terminator);

    // Call the reference processing task's work routine.
    _proc_task.work(i, is_alive, keep_alive, drain_queue);

    // Note we cannot assert that the refs array is empty here as not all
    // of the processing tasks (specifically phase2 - pp2_work) execute
    // the complete_gc closure (which ordinarily would drain the queue) so
    // the queue may not be empty.
  }
};

// Driver routine for parallel reference processing.
// Creates an instance of the ref processing gang
// task and has the worker threads execute it.
void G1STWRefProcTaskExecutor::execute(ProcessTask& proc_task) {
  assert(_workers != NULL, "Need parallel worker threads.");

  ParallelTaskTerminator terminator(_active_workers, _queues);
  G1STWRefProcTaskProxy proc_task_proxy(proc_task, _g1h, _queues, &terminator);

  _g1h->set_par_threads(_active_workers);
  _workers->run_task(&proc_task_proxy);
  _g1h->set_par_threads(0);
}

// Gang task for parallel reference enqueueing.

class G1STWRefEnqueueTaskProxy: public AbstractGangTask {
  typedef AbstractRefProcTaskExecutor::EnqueueTask EnqueueTask;
  EnqueueTask& _enq_task;

public:
  G1STWRefEnqueueTaskProxy(EnqueueTask& enq_task) :
    AbstractGangTask("Enqueue reference objects in parallel"),
    _enq_task(enq_task)
  { }

  virtual void work(int i) {
    _enq_task.work(i);
  }
};

// Driver routine for parallel reference enqueing.
// Creates an instance of the ref enqueueing gang
// task and has the worker threads execute it.

void G1STWRefProcTaskExecutor::execute(EnqueueTask& enq_task) {
  assert(_workers != NULL, "Need parallel worker threads.");

  G1STWRefEnqueueTaskProxy enq_task_proxy(enq_task);

  _g1h->set_par_threads(_active_workers);
  _workers->run_task(&enq_task_proxy);
  _g1h->set_par_threads(0);
}

// End of weak reference support closures

// Abstract task used to preserve (i.e. copy) any referent objects
// that are in the collection set and are pointed to by reference
// objects discovered by the CM ref processor.

class G1ParPreserveCMReferentsTask: public AbstractGangTask {
protected:
  G1CollectedHeap* _g1h;
  RefToScanQueueSet      *_queues;
  ParallelTaskTerminator _terminator;
  int _n_workers;

public:
  G1ParPreserveCMReferentsTask(G1CollectedHeap* g1h,int workers, RefToScanQueueSet *task_queues) :
    AbstractGangTask("ParPreserveCMReferents"),
    _g1h(g1h),
    _queues(task_queues),
    _terminator(workers, _queues),
    _n_workers(workers)
  { }

  void work(int i) {
    ResourceMark rm;
    HandleMark   hm;

    G1ParScanThreadState            pss(_g1h, i);
    G1ParScanHeapEvacClosure        scan_evac_cl(_g1h, &pss, NULL);
    G1ParScanHeapEvacFailureClosure evac_failure_cl(_g1h, &pss, NULL);
    G1ParScanPartialArrayClosure    partial_scan_cl(_g1h, &pss, NULL);

    pss.set_evac_closure(&scan_evac_cl);
    pss.set_evac_failure_closure(&evac_failure_cl);
    pss.set_partial_scan_closure(&partial_scan_cl);

    assert(pss.refs()->is_empty(), "both queue and overflow should be empty");


    G1ParScanExtRootClosure        only_copy_non_heap_cl(_g1h, &pss, NULL);
    G1ParScanPermClosure           only_copy_perm_cl(_g1h, &pss, NULL);

    G1ParScanAndMarkExtRootClosure copy_mark_non_heap_cl(_g1h, &pss, NULL);
    G1ParScanAndMarkPermClosure    copy_mark_perm_cl(_g1h, &pss, NULL);

    OopClosure*                    copy_non_heap_cl = &only_copy_non_heap_cl;
    OopsInHeapRegionClosure*       copy_perm_cl = &only_copy_perm_cl;

    if (_g1h->g1_policy()->during_initial_mark_pause()) {
      // We also need to mark copied objects.
      copy_non_heap_cl = &copy_mark_non_heap_cl;
      copy_perm_cl = &copy_mark_perm_cl;
    }

    // Is alive closure
    G1AlwaysAliveClosure always_alive(_g1h);

    // Copying keep alive closure. Applied to referent objects that need
    // to be copied.
    G1CopyingKeepAliveClosure keep_alive(_g1h, copy_non_heap_cl, copy_perm_cl, &pss);

    ReferenceProcessor* rp = _g1h->ref_processor_cm();

    int limit = ReferenceProcessor::number_of_subclasses_of_ref() * rp->max_num_q();
    int stride = MIN2(MAX2(_n_workers, 1), limit);

    // limit is set using max_num_q() - which was set using ParallelGCThreads.
    // So this must be true - but assert just in case someone decides to
    // change the worker ids.
    assert(0 <= i && i < limit, "sanity");
    assert(!rp->discovery_is_atomic(), "check this code");

    // Select discovered lists [i, i+stride, i+2*stride,...,limit)
    for (int idx = i; idx < limit; idx += stride) {
      DiscoveredList& ref_list = rp->discovered_refs()[idx];

      DiscoveredListIterator iter(ref_list, &keep_alive, &always_alive);
      while (iter.has_next()) {
        // Since discovery is not atomic for the CM ref processor, we
        // can see some null referent objects.
        iter.load_ptrs(DEBUG_ONLY(true));
        oop ref = iter.obj();

        // This will filter nulls.
        if (iter.is_referent_alive()) {
          iter.make_referent_alive();
        }
        iter.move_to_next();
      }
    }

    // Drain the queue - which may cause stealing
    G1ParEvacuateFollowersClosure drain_queue(_g1h, &pss, _queues, &_terminator);
    drain_queue.do_void();
    // Allocation buffers were retired at the end of G1ParEvacuateFollowersClosure
    assert(pss.refs()->is_empty(), "should be");
  }
};

// Weak Reference processing during an evacuation pause (part 1).
void G1CollectedHeap::process_discovered_references() {
  double ref_proc_start = os::elapsedTime();

  ReferenceProcessor* rp = _ref_processor_stw;
  assert(rp->discovery_enabled(), "should have been enabled");

  // Any reference objects, in the collection set, that were 'discovered'
  // by the CM ref processor should have already been copied (either by
  // applying the external root copy closure to the discovered lists, or
  // by following an RSet entry).
  //
  // But some of the referents, that are in the collection set, that these
  // reference objects point to may not have been copied: the STW ref
  // processor would have seen that the reference object had already
  // been 'discovered' and would have skipped discovering the reference,
  // but would not have treated the reference object as a regular oop.
  // As a reult the copy closure would not have been applied to the
  // referent object.
  //
  // We need to explicitly copy these referent objects - the references
  // will be processed at the end of remarking.
  //
  // We also need to do this copying before we process the reference
  // objects discovered by the STW ref processor in case one of these
  // referents points to another object which is also referenced by an
  // object discovered by the STW ref processor.

  int n_workers = (G1CollectedHeap::use_parallel_gc_threads() ?
                        workers()->total_workers() : 1);

  set_par_threads(n_workers);
  G1ParPreserveCMReferentsTask keep_cm_referents(this, n_workers, _task_queues);

  if (G1CollectedHeap::use_parallel_gc_threads()) {
    workers()->run_task(&keep_cm_referents);
  } else {
    keep_cm_referents.work(0);
  }

  set_par_threads(0);

  // Closure to test whether a referent is alive.
  G1STWIsAliveClosure is_alive(this);

  // Even when parallel reference processing is enabled, the processing
  // of JNI refs is serial and performed serially by the current thread
  // rather than by a worker. The following PSS will be used for processing
  // JNI refs.

  // Use only a single queue for this PSS.
  G1ParScanThreadState pss(this, 0);

  // We do not embed a reference processor in the copying/scanning
  // closures while we're actually processing the discovered
  // reference objects.
  G1ParScanHeapEvacClosure        scan_evac_cl(this, &pss, NULL);
  G1ParScanHeapEvacFailureClosure evac_failure_cl(this, &pss, NULL);
  G1ParScanPartialArrayClosure    partial_scan_cl(this, &pss, NULL);

  pss.set_evac_closure(&scan_evac_cl);
  pss.set_evac_failure_closure(&evac_failure_cl);
  pss.set_partial_scan_closure(&partial_scan_cl);

  assert(pss.refs()->is_empty(), "pre-condition");

  G1ParScanExtRootClosure        only_copy_non_heap_cl(this, &pss, NULL);
  G1ParScanPermClosure           only_copy_perm_cl(this, &pss, NULL);

  G1ParScanAndMarkExtRootClosure copy_mark_non_heap_cl(this, &pss, NULL);
  G1ParScanAndMarkPermClosure    copy_mark_perm_cl(this, &pss, NULL);

  OopClosure*                    copy_non_heap_cl = &only_copy_non_heap_cl;
  OopsInHeapRegionClosure*       copy_perm_cl = &only_copy_perm_cl;

  if (_g1h->g1_policy()->during_initial_mark_pause()) {
    // We also need to mark copied objects.
    copy_non_heap_cl = &copy_mark_non_heap_cl;
    copy_perm_cl = &copy_mark_perm_cl;
  }

  // Keep alive closure.
  G1CopyingKeepAliveClosure keep_alive(this, copy_non_heap_cl, copy_perm_cl, &pss);

  // Serial Complete GC closure
  G1STWDrainQueueClosure drain_queue(this, &pss);

  // Setup the soft refs policy...
  rp->setup_policy(false);

  if (!rp->processing_is_mt()) {
    // Serial reference processing...
    rp->process_discovered_references(&is_alive,
                                      &keep_alive,
                                      &drain_queue,
                                      NULL);
  } else {
    // Parallel reference processing
    int active_workers = (ParallelGCThreads > 0 ? workers()->total_workers() : 1);
    assert(rp->num_q() == active_workers, "sanity");
    assert(active_workers <= rp->max_num_q(), "sanity");

    G1STWRefProcTaskExecutor par_task_executor(this, workers(), _task_queues, active_workers);
    rp->process_discovered_references(&is_alive, &keep_alive, &drain_queue, &par_task_executor);
  }

  // We have completed copying any necessary live referent objects
  // (that were not copied during the actual pause) so we can
  // retire any active alloc buffers
  pss.retire_alloc_buffers();
  assert(pss.refs()->is_empty(), "both queue and overflow should be empty");

  double ref_proc_time = os::elapsedTime() - ref_proc_start;
  g1_policy()->record_ref_proc_time(ref_proc_time * 1000.0);
}

// Weak Reference processing during an evacuation pause (part 2).
void G1CollectedHeap::enqueue_discovered_references() {
  double ref_enq_start = os::elapsedTime();

  ReferenceProcessor* rp = _ref_processor_stw;
  assert(!rp->discovery_enabled(), "should have been disabled as part of processing");

  // Now enqueue any remaining on the discovered lists on to
  // the pending list.
  if (!rp->processing_is_mt()) {
    // Serial reference processing...
    rp->enqueue_discovered_references();
  } else {
    // Parallel reference enqueuing

    int active_workers = (ParallelGCThreads > 0 ? workers()->total_workers() : 1);
    assert(rp->num_q() == active_workers, "sanity");
    assert(active_workers <= rp->max_num_q(), "sanity");

    G1STWRefProcTaskExecutor par_task_executor(this, workers(), _task_queues, active_workers);
    rp->enqueue_discovered_references(&par_task_executor);
  }

  rp->verify_no_references_recorded();
  assert(!rp->discovery_enabled(), "should have been disabled");

  // FIXME
  // CM's reference processing also cleans up the string and symbol tables.
  // Should we do that here also? We could, but it is a serial operation
  // and could signicantly increase the pause time.

  double ref_enq_time = os::elapsedTime() - ref_enq_start;
  g1_policy()->record_ref_enq_time(ref_enq_time * 1000.0);
}

void G1CollectedHeap::evacuate_collection_set() {
  set_evacuation_failed(false);

  g1_rem_set()->prepare_for_oops_into_collection_set_do();
  concurrent_g1_refine()->set_use_cache(false);
  concurrent_g1_refine()->clear_hot_cache_claimed_index();

  int n_workers = (ParallelGCThreads > 0 ? workers()->total_workers() : 1);
  set_par_threads(n_workers);
  G1ParTask g1_par_task(this, n_workers, _task_queues);

  init_for_evac_failure(NULL);

  rem_set()->prepare_for_younger_refs_iterate(true);

  assert(dirty_card_queue_set().completed_buffers_num() == 0, "Should be empty");
  double start_par = os::elapsedTime();

  if (G1CollectedHeap::use_parallel_gc_threads()) {
    // The individual threads will set their evac-failure closures.
    StrongRootsScope srs(this);
    if (ParallelGCVerbose) G1ParScanThreadState::print_termination_stats_hdr();
    workers()->run_task(&g1_par_task);
  } else {
    StrongRootsScope srs(this);
    g1_par_task.work(0);
  }

  double par_time = (os::elapsedTime() - start_par) * 1000.0;
  g1_policy()->record_par_time(par_time);
  set_par_threads(0);

  // Process any discovered reference objects - we have
  // to do this _before_ we retire the GC alloc regions
  // as we may have to copy some 'reachable' referent
  // objects (and their reachable sub-graphs) that were
  // not copied during the pause.
  process_discovered_references();

  // Weak root processing.
  // Note: when JSR 292 is enabled and code blobs can contain
  // non-perm oops then we will need to process the code blobs
  // here too.
  {
    G1STWIsAliveClosure is_alive(this);
    G1KeepAliveClosure keep_alive(this);
    JNIHandles::weak_oops_do(&is_alive, &keep_alive);
  }

  release_gc_alloc_regions();
  g1_rem_set()->cleanup_after_oops_into_collection_set_do();

  concurrent_g1_refine()->clear_hot_cache();
  concurrent_g1_refine()->set_use_cache(true);

  finalize_for_evac_failure();

  // Must do this before removing self-forwarding pointers, which clears
  // the per-region evac-failure flags.
  concurrent_mark()->complete_marking_in_collection_set();

  if (evacuation_failed()) {
    remove_self_forwarding_pointers();
    if (PrintGCDetails) {
      gclog_or_tty->print(" (to-space overflow)");
    } else if (PrintGC) {
      gclog_or_tty->print("--");
    }
  }

  // Enqueue any remaining references remaining on the STW
  // reference processor's discovered lists. We need to do
  // this after the card table is cleaned (and verified) as
  // the act of enqueuing entries on to the pending list
  // will log these updates (and dirty their associated
  // cards). We need these updates logged to update any
  // RSets.
  enqueue_discovered_references();

  if (G1DeferredRSUpdate) {
    RedirtyLoggedCardTableEntryFastClosure redirty;
    dirty_card_queue_set().set_closure(&redirty);
    dirty_card_queue_set().apply_closure_to_all_completed_buffers();

    DirtyCardQueueSet& dcq = JavaThread::dirty_card_queue_set();
    dcq.merge_bufferlists(&dirty_card_queue_set());
    assert(dirty_card_queue_set().completed_buffers_num() == 0, "All should be consumed");
  }
  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
}

void G1CollectedHeap::free_region_if_empty(HeapRegion* hr,
                                     size_t* pre_used,
                                     FreeRegionList* free_list,
                                     HumongousRegionSet* humongous_proxy_set,
                                     HRRSCleanupTask* hrrs_cleanup_task,
                                     bool par) {
  if (hr->used() > 0 && hr->max_live_bytes() == 0 && !hr->is_young()) {
    if (hr->isHumongous()) {
      assert(hr->startsHumongous(), "we should only see starts humongous");
      free_humongous_region(hr, pre_used, free_list, humongous_proxy_set, par);
    } else {
      free_region(hr, pre_used, free_list, par);
    }
  } else {
    hr->rem_set()->do_cleanup_work(hrrs_cleanup_task);
  }
}

void G1CollectedHeap::free_region(HeapRegion* hr,
                                  size_t* pre_used,
                                  FreeRegionList* free_list,
                                  bool par) {
  assert(!hr->isHumongous(), "this is only for non-humongous regions");
  assert(!hr->is_empty(), "the region should not be empty");
  assert(free_list != NULL, "pre-condition");

  *pre_used += hr->used();
  hr->hr_clear(par, true /* clear_space */);
  free_list->add_as_head(hr);
}

void G1CollectedHeap::free_humongous_region(HeapRegion* hr,
                                     size_t* pre_used,
                                     FreeRegionList* free_list,
                                     HumongousRegionSet* humongous_proxy_set,
                                     bool par) {
  assert(hr->startsHumongous(), "this is only for starts humongous regions");
  assert(free_list != NULL, "pre-condition");
  assert(humongous_proxy_set != NULL, "pre-condition");

  size_t hr_used = hr->used();
  size_t hr_capacity = hr->capacity();
  size_t hr_pre_used = 0;
  _humongous_set.remove_with_proxy(hr, humongous_proxy_set);
  hr->set_notHumongous();
  free_region(hr, &hr_pre_used, free_list, par);

  size_t i = hr->hrs_index() + 1;
  size_t num = 1;
  while (i < n_regions()) {
    HeapRegion* curr_hr = region_at(i);
    if (!curr_hr->continuesHumongous()) {
      break;
    }
    curr_hr->set_notHumongous();
    free_region(curr_hr, &hr_pre_used, free_list, par);
    num += 1;
    i += 1;
  }
  assert(hr_pre_used == hr_used,
         err_msg("hr_pre_used: "SIZE_FORMAT" and hr_used: "SIZE_FORMAT" "
                 "should be the same", hr_pre_used, hr_used));
  *pre_used += hr_pre_used;
}

void G1CollectedHeap::update_sets_after_freeing_regions(size_t pre_used,
                                       FreeRegionList* free_list,
                                       HumongousRegionSet* humongous_proxy_set,
                                       bool par) {
  if (pre_used > 0) {
    Mutex* lock = (par) ? ParGCRareEvent_lock : NULL;
    MutexLockerEx x(lock, Mutex::_no_safepoint_check_flag);
    assert(_summary_bytes_used >= pre_used,
           err_msg("invariant: _summary_bytes_used: "SIZE_FORMAT" "
                   "should be >= pre_used: "SIZE_FORMAT,
                   _summary_bytes_used, pre_used));
    _summary_bytes_used -= pre_used;
  }
  if (free_list != NULL && !free_list->is_empty()) {
    MutexLockerEx x(FreeList_lock, Mutex::_no_safepoint_check_flag);
    _free_list.add_as_head(free_list);
  }
  if (humongous_proxy_set != NULL && !humongous_proxy_set->is_empty()) {
    MutexLockerEx x(OldSets_lock, Mutex::_no_safepoint_check_flag);
    _humongous_set.update_from_proxy(humongous_proxy_set);
  }
}

class G1ParCleanupCTTask : public AbstractGangTask {
  CardTableModRefBS* _ct_bs;
  G1CollectedHeap* _g1h;
  HeapRegion* volatile _su_head;
public:
  G1ParCleanupCTTask(CardTableModRefBS* ct_bs,
                     G1CollectedHeap* g1h) :
    AbstractGangTask("G1 Par Cleanup CT Task"),
    _ct_bs(ct_bs), _g1h(g1h) { }

  void work(int i) {
    HeapRegion* r;
    while (r = _g1h->pop_dirty_cards_region()) {
      clear_cards(r);
    }
  }

  void clear_cards(HeapRegion* r) {
    // Cards of the survivors should have already been dirtied.
    if (!r->is_survivor()) {
      _ct_bs->clear(MemRegion(r->bottom(), r->end()));
    }
  }
};

#ifndef PRODUCT
class G1VerifyCardTableCleanup: public HeapRegionClosure {
  G1CollectedHeap* _g1h;
  CardTableModRefBS* _ct_bs;
public:
  G1VerifyCardTableCleanup(G1CollectedHeap* g1h, CardTableModRefBS* ct_bs)
    : _g1h(g1h), _ct_bs(ct_bs) { }
  virtual bool doHeapRegion(HeapRegion* r) {
    if (r->is_survivor()) {
      _g1h->verify_dirty_region(r);
    } else {
      _g1h->verify_not_dirty_region(r);
    }
    return false;
  }
};

void G1CollectedHeap::verify_not_dirty_region(HeapRegion* hr) {
  // All of the region should be clean.
  CardTableModRefBS* ct_bs = (CardTableModRefBS*)barrier_set();
  MemRegion mr(hr->bottom(), hr->end());
  ct_bs->verify_not_dirty_region(mr);
}

void G1CollectedHeap::verify_dirty_region(HeapRegion* hr) {
  // We cannot guarantee that [bottom(),end()] is dirty.  Threads
  // dirty allocated blocks as they allocate them. The thread that
  // retires each region and replaces it with a new one will do a
  // maximal allocation to fill in [pre_dummy_top(),end()] but will
  // not dirty that area (one less thing to have to do while holding
  // a lock). So we can only verify that [bottom(),pre_dummy_top()]
  // is dirty.
  CardTableModRefBS* ct_bs = (CardTableModRefBS*) barrier_set();
  MemRegion mr(hr->bottom(), hr->pre_dummy_top());
  ct_bs->verify_dirty_region(mr);
}

void G1CollectedHeap::verify_dirty_young_list(HeapRegion* head) {
  CardTableModRefBS* ct_bs = (CardTableModRefBS*) barrier_set();
  for (HeapRegion* hr = head; hr != NULL; hr = hr->get_next_young_region()) {
    verify_dirty_region(hr);
  }
}

void G1CollectedHeap::verify_dirty_young_regions() {
  verify_dirty_young_list(_young_list->first_region());
  verify_dirty_young_list(_young_list->first_survivor_region());
}
#endif

void G1CollectedHeap::cleanUpCardTable() {
  CardTableModRefBS* ct_bs = (CardTableModRefBS*) (barrier_set());
  double start = os::elapsedTime();

  // Iterate over the dirty cards region list.
  G1ParCleanupCTTask cleanup_task(ct_bs, this);

  if (ParallelGCThreads > 0) {
    set_par_threads(workers()->total_workers());
    workers()->run_task(&cleanup_task);
    set_par_threads(0);
  } else {
    while (_dirty_cards_region_list) {
      HeapRegion* r = _dirty_cards_region_list;
      cleanup_task.clear_cards(r);
      _dirty_cards_region_list = r->get_next_dirty_cards_region();
      if (_dirty_cards_region_list == r) {
        // The last region.
        _dirty_cards_region_list = NULL;
      }
      r->set_next_dirty_cards_region(NULL);
    }
  }

  double elapsed = os::elapsedTime() - start;
  g1_policy()->record_clear_ct_time(elapsed * 1000.0);
#ifndef PRODUCT
  if (G1VerifyCTCleanup || VerifyAfterGC) {
    G1VerifyCardTableCleanup cleanup_verifier(this, ct_bs);
    heap_region_iterate(&cleanup_verifier);
  }
#endif
}

void G1CollectedHeap::free_collection_set(HeapRegion* cs_head) {
  size_t pre_used = 0;
  FreeRegionList local_free_list("Local List for CSet Freeing");

  double young_time_ms     = 0.0;
  double non_young_time_ms = 0.0;

  // Since the collection set is a superset of the the young list,
  // all we need to do to clear the young list is clear its
  // head and length, and unlink any young regions in the code below
  _young_list->clear();

  G1CollectorPolicy* policy = g1_policy();

  double start_sec = os::elapsedTime();
  bool non_young = true;

  HeapRegion* cur = cs_head;
  int age_bound = -1;
  size_t rs_lengths = 0;

  while (cur != NULL) {
    assert(!is_on_master_free_list(cur), "sanity");

    if (non_young) {
      if (cur->is_young()) {
        double end_sec = os::elapsedTime();
        double elapsed_ms = (end_sec - start_sec) * 1000.0;
        non_young_time_ms += elapsed_ms;

        start_sec = os::elapsedTime();
        non_young = false;
      }
    } else {
      double end_sec = os::elapsedTime();
      double elapsed_ms = (end_sec - start_sec) * 1000.0;
      young_time_ms += elapsed_ms;

      start_sec = os::elapsedTime();
      non_young = true;
    }

    rs_lengths += cur->rem_set()->occupied();

    HeapRegion* next = cur->next_in_collection_set();
    assert(cur->in_collection_set(), "bad CS");
    cur->set_next_in_collection_set(NULL);
    cur->set_in_collection_set(false);

    if (cur->is_young()) {
      int index = cur->young_index_in_cset();
      guarantee( index != -1, "invariant" );
      guarantee( (size_t)index < policy->young_cset_length(), "invariant" );
      size_t words_survived = _surviving_young_words[index];
      cur->record_surv_words_in_group(words_survived);

      // At this point the we have 'popped' cur from the collection set
      // (linked via next_in_collection_set()) but it is still in the
      // young list (linked via next_young_region()). Clear the
      // _next_young_region field.
      cur->set_next_young_region(NULL);
    } else {
      int index = cur->young_index_in_cset();
      guarantee( index == -1, "invariant" );
    }

    assert( (cur->is_young() && cur->young_index_in_cset() > -1) ||
            (!cur->is_young() && cur->young_index_in_cset() == -1),
            "invariant" );

    if (!cur->evacuation_failed()) {
      // And the region is empty.
      assert(!cur->is_empty(), "Should not have empty regions in a CS.");
      free_region(cur, &pre_used, &local_free_list, false /* par */);
    } else {
      cur->uninstall_surv_rate_group();
      if (cur->is_young())
        cur->set_young_index_in_cset(-1);
      cur->set_not_young();
      cur->set_evacuation_failed(false);
    }
    cur = next;
  }

  policy->record_max_rs_lengths(rs_lengths);
  policy->cset_regions_freed();

  double end_sec = os::elapsedTime();
  double elapsed_ms = (end_sec - start_sec) * 1000.0;
  if (non_young)
    non_young_time_ms += elapsed_ms;
  else
    young_time_ms += elapsed_ms;

  update_sets_after_freeing_regions(pre_used, &local_free_list,
                                    NULL /* humongous_proxy_set */,
                                    false /* par */);
  policy->record_young_free_cset_time_ms(young_time_ms);
  policy->record_non_young_free_cset_time_ms(non_young_time_ms);
}

// This routine is similar to the above but does not record
// any policy statistics or update free lists; we are abandoning
// the current incremental collection set in preparation of a
// full collection. After the full GC we will start to build up
// the incremental collection set again.
// This is only called when we're doing a full collection
// and is immediately followed by the tearing down of the young list.

void G1CollectedHeap::abandon_collection_set(HeapRegion* cs_head) {
  HeapRegion* cur = cs_head;

  while (cur != NULL) {
    HeapRegion* next = cur->next_in_collection_set();
    assert(cur->in_collection_set(), "bad CS");
    cur->set_next_in_collection_set(NULL);
    cur->set_in_collection_set(false);
    cur->set_young_index_in_cset(-1);
    cur = next;
  }
}

void G1CollectedHeap::set_free_regions_coming() {
  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [cm thread] : "
                           "setting free regions coming");
  }

  assert(!free_regions_coming(), "pre-condition");
  _free_regions_coming = true;
}

void G1CollectedHeap::reset_free_regions_coming() {
  {
    assert(free_regions_coming(), "pre-condition");
    MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
    _free_regions_coming = false;
    SecondaryFreeList_lock->notify_all();
  }

  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [cm thread] : "
                           "reset free regions coming");
  }
}

void G1CollectedHeap::wait_while_free_regions_coming() {
  // Most of the time we won't have to wait, so let's do a quick test
  // first before we take the lock.
  if (!free_regions_coming()) {
    return;
  }

  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [other] : "
                           "waiting for free regions");
  }

  {
    MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
    while (free_regions_coming()) {
      SecondaryFreeList_lock->wait(Mutex::_no_safepoint_check_flag);
    }
  }

  if (G1ConcRegionFreeingVerbose) {
    gclog_or_tty->print_cr("G1ConcRegionFreeing [other] : "
                           "done waiting for free regions");
  }
}

void G1CollectedHeap::set_region_short_lived_locked(HeapRegion* hr) {
  assert(heap_lock_held_for_gc(),
              "the heap lock should already be held by or for this thread");
  _young_list->push_region(hr);
  g1_policy()->set_region_short_lived(hr);
}

class NoYoungRegionsClosure: public HeapRegionClosure {
private:
  bool _success;
public:
  NoYoungRegionsClosure() : _success(true) { }
  bool doHeapRegion(HeapRegion* r) {
    if (r->is_young()) {
      gclog_or_tty->print_cr("Region ["PTR_FORMAT", "PTR_FORMAT") tagged as young",
                             r->bottom(), r->end());
      _success = false;
    }
    return false;
  }
  bool success() { return _success; }
};

bool G1CollectedHeap::check_young_list_empty(bool check_heap, bool check_sample) {
  bool ret = _young_list->check_list_empty(check_sample);

  if (check_heap) {
    NoYoungRegionsClosure closure;
    heap_region_iterate(&closure);
    ret = ret && closure.success();
  }

  return ret;
}

void G1CollectedHeap::empty_young_list() {
  assert(heap_lock_held_for_gc(),
              "the heap lock should already be held by or for this thread");

  _young_list->empty_list();
}

// Done at the start of full GC.
void G1CollectedHeap::tear_down_region_lists() {
  _free_list.remove_all();
}

class RegionResetter: public HeapRegionClosure {
  G1CollectedHeap* _g1h;
  FreeRegionList _local_free_list;

public:
  RegionResetter() : _g1h(G1CollectedHeap::heap()),
                     _local_free_list("Local Free List for RegionResetter") { }

  bool doHeapRegion(HeapRegion* r) {
    if (r->continuesHumongous()) return false;
    if (r->top() > r->bottom()) {
      if (r->top() < r->end()) {
        Copy::fill_to_words(r->top(),
                          pointer_delta(r->end(), r->top()));
      }
    } else {
      assert(r->is_empty(), "tautology");
      _local_free_list.add_as_tail(r);
    }
    return false;
  }

  void update_free_lists() {
    _g1h->update_sets_after_freeing_regions(0, &_local_free_list, NULL,
                                            false /* par */);
  }
};

// Done at the end of full GC.
void G1CollectedHeap::rebuild_region_lists() {
  // This needs to go at the end of the full GC.
  RegionResetter rs;
  heap_region_iterate(&rs);
  rs.update_free_lists();
}

void G1CollectedHeap::set_refine_cte_cl_concurrency(bool concurrent) {
  _refine_cte_cl->set_concurrent(concurrent);
}

bool G1CollectedHeap::is_in_closed_subset(const void* p) const {
  HeapRegion* hr = heap_region_containing(p);
  if (hr == NULL) {
    return is_in_permanent(p);
  } else {
    return hr->is_in(p);
  }
}

// Methods for the mutator alloc region

HeapRegion* G1CollectedHeap::new_mutator_alloc_region(size_t word_size,
                                                      bool force) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  assert(!force || g1_policy()->can_expand_young_list(),
         "if force is true we should be able to expand the young list");
  bool young_list_full = g1_policy()->is_young_list_full();
  if (force || !young_list_full) {
    HeapRegion* new_alloc_region = new_region(word_size,
                                              false /* do_expand */);
    if (new_alloc_region != NULL) {
      g1_policy()->update_region_num(true /* next_is_young */);
      set_region_short_lived_locked(new_alloc_region);
      _hr_printer.alloc(new_alloc_region, G1HRPrinter::Eden, young_list_full);
      return new_alloc_region;
    }
  }
  return NULL;
}

void G1CollectedHeap::retire_mutator_alloc_region(HeapRegion* alloc_region,
                                                  size_t allocated_bytes) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  assert(alloc_region->is_young(), "all mutator alloc regions should be young");

  g1_policy()->add_region_to_incremental_cset_lhs(alloc_region);
  _summary_bytes_used += allocated_bytes;
  _hr_printer.retire(alloc_region);
  // We update the eden sizes here, when the region is retired,
  // instead of when it's allocated, since this is the point that its
  // used space has been recored in _summary_bytes_used.
  g1mm()->update_eden_size();
}

HeapRegion* MutatorAllocRegion::allocate_new_region(size_t word_size,
                                                    bool force) {
  return _g1h->new_mutator_alloc_region(word_size, force);
}

void MutatorAllocRegion::retire_region(HeapRegion* alloc_region,
                                       size_t allocated_bytes) {
  _g1h->retire_mutator_alloc_region(alloc_region, allocated_bytes);
}

// Methods for the GC alloc regions

HeapRegion* G1CollectedHeap::new_gc_alloc_region(size_t word_size,
                                                 size_t count,
                                                 GCAllocPurpose ap) {
  assert(FreeList_lock->owned_by_self(), "pre-condition");

  if (count < g1_policy()->max_regions(ap)) {
    HeapRegion* new_alloc_region = new_region(word_size,
                                              true /* do_expand */);
    if (new_alloc_region != NULL) {
      // We really only need to do this for old regions given that we
      // should never scan survivors. But it doesn't hurt to do it
      // for survivors too.
      new_alloc_region->set_saved_mark();
      if (ap == GCAllocForSurvived) {
        new_alloc_region->set_survivor();
        _hr_printer.alloc(new_alloc_region, G1HRPrinter::Survivor);
      } else {
        _hr_printer.alloc(new_alloc_region, G1HRPrinter::Old);
      }
      return new_alloc_region;
    } else {
      g1_policy()->note_alloc_region_limit_reached(ap);
    }
  }
  return NULL;
}

void G1CollectedHeap::retire_gc_alloc_region(HeapRegion* alloc_region,
                                             size_t allocated_bytes,
                                             GCAllocPurpose ap) {
  alloc_region->note_end_of_copying();
  g1_policy()->record_bytes_copied_during_gc(allocated_bytes);
  if (ap == GCAllocForSurvived) {
    young_list()->add_survivor_region(alloc_region);
  }
  _hr_printer.retire(alloc_region);
}

HeapRegion* SurvivorGCAllocRegion::allocate_new_region(size_t word_size,
                                                       bool force) {
  assert(!force, "not supported for GC alloc regions");
  return _g1h->new_gc_alloc_region(word_size, count(), GCAllocForSurvived);
}

void SurvivorGCAllocRegion::retire_region(HeapRegion* alloc_region,
                                          size_t allocated_bytes) {
  _g1h->retire_gc_alloc_region(alloc_region, allocated_bytes,
                               GCAllocForSurvived);
}

HeapRegion* OldGCAllocRegion::allocate_new_region(size_t word_size,
                                                  bool force) {
  assert(!force, "not supported for GC alloc regions");
  return _g1h->new_gc_alloc_region(word_size, count(), GCAllocForTenured);
}

void OldGCAllocRegion::retire_region(HeapRegion* alloc_region,
                                     size_t allocated_bytes) {
  _g1h->retire_gc_alloc_region(alloc_region, allocated_bytes,
                               GCAllocForTenured);
}
// Heap region set verification

class VerifyRegionListsClosure : public HeapRegionClosure {
private:
  HumongousRegionSet* _humongous_set;
  FreeRegionList*     _free_list;
  size_t              _region_count;

public:
  VerifyRegionListsClosure(HumongousRegionSet* humongous_set,
                           FreeRegionList* free_list) :
    _humongous_set(humongous_set), _free_list(free_list),
    _region_count(0) { }

  size_t region_count()      { return _region_count;      }

  bool doHeapRegion(HeapRegion* hr) {
    _region_count += 1;

    if (hr->continuesHumongous()) {
      return false;
    }

    if (hr->is_young()) {
      // TODO
    } else if (hr->startsHumongous()) {
      _humongous_set->verify_next_region(hr);
    } else if (hr->is_empty()) {
      _free_list->verify_next_region(hr);
    }
    return false;
  }
};

HeapRegion* G1CollectedHeap::new_heap_region(size_t hrs_index,
                                             HeapWord* bottom) {
  HeapWord* end = bottom + HeapRegion::GrainWords;
  MemRegion mr(bottom, end);
  assert(_g1_reserved.contains(mr), "invariant");
  // This might return NULL if the allocation fails
  return new HeapRegion(hrs_index, _bot_shared, mr, true /* is_zeroed */);
}

void G1CollectedHeap::verify_region_sets() {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);

  // First, check the explicit lists.
  _free_list.verify();
  {
    // Given that a concurrent operation might be adding regions to
    // the secondary free list we have to take the lock before
    // verifying it.
    MutexLockerEx x(SecondaryFreeList_lock, Mutex::_no_safepoint_check_flag);
    _secondary_free_list.verify();
  }
  _humongous_set.verify();

  // If a concurrent region freeing operation is in progress it will
  // be difficult to correctly attributed any free regions we come
  // across to the correct free list given that they might belong to
  // one of several (free_list, secondary_free_list, any local lists,
  // etc.). So, if that's the case we will skip the rest of the
  // verification operation. Alternatively, waiting for the concurrent
  // operation to complete will have a non-trivial effect on the GC's
  // operation (no concurrent operation will last longer than the
  // interval between two calls to verification) and it might hide
  // any issues that we would like to catch during testing.
  if (free_regions_coming()) {
    return;
  }

  // Make sure we append the secondary_free_list on the free_list so
  // that all free regions we will come across can be safely
  // attributed to the free_list.
  append_secondary_free_list_if_not_empty_with_lock();

  // Finally, make sure that the region accounting in the lists is
  // consistent with what we see in the heap.
  _humongous_set.verify_start();
  _free_list.verify_start();

  VerifyRegionListsClosure cl(&_humongous_set, &_free_list);
  heap_region_iterate(&cl);

  _humongous_set.verify_end();
  _free_list.verify_end();
}
