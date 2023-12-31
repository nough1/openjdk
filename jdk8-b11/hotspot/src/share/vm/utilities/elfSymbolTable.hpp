/*
 * Copyright (c) 1997, 2010, Oracle and/or its affiliates. All rights reserved.
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

#ifndef __ELF_SYMBOL_TABLE_HPP
#define __ELF_SYMBOL_TABLE_HPP

#if !defined(_WINDOWS) && !defined(__APPLE__)


#include "memory/allocation.hpp"
#include "utilities/decoder.hpp"
#include "utilities/elfFile.hpp"

/*
 * symbol table object represents a symbol section in an elf file.
 * Whenever possible, it will load all symbols from the corresponding section
 * of the elf file into memory. Otherwise, it will walk the section in file
 * to look up the symbol that nearest the given address.
 */
class ElfSymbolTable: public CHeapObj {
  friend class ElfFile;
 public:
  ElfSymbolTable(FILE* file, Elf_Shdr shdr);
  ~ElfSymbolTable();

  // search the symbol that is nearest to the specified address.
  Decoder::decoder_status lookup(address addr, int* stringtableIndex, int* posIndex, int* offset);

  Decoder::decoder_status get_status() { return m_status; };

 protected:
  ElfSymbolTable*  m_next;

  // holds a complete symbol table section if
  // can allocate enough memory
  Elf_Sym*            m_symbols;

  // file contains string table
  FILE*               m_file;

  // section header
  Elf_Shdr            m_shdr;

  Decoder::decoder_status  m_status;
};

#endif // _WINDOWS

#endif // __ELF_SYMBOL_TABLE_HPP
