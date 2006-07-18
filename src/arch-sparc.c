/* Copyright (C) 2001 Red Hat, Inc.
   Written by Jakub Jelinek <jakub@redhat.com>, 2001.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <error.h>
#include <argp.h>
#include <stdlib.h>

#include "prelink.h"

static int
sparc_adjust_dyn (DSO *dso, int n, GElf_Dyn *dyn, GElf_Addr start,
		 GElf_Addr adjust)
{
  if (dyn->d_tag == DT_PLTGOT)
    {
      int i;

      for (i = 1; i < dso->ehdr.e_shnum; ++i)
	if (! strcmp (strptr (dso, dso->ehdr.e_shstrndx,
			      dso->shdr[i].sh_name), ".got"))
	  {
	    Elf32_Addr data;

	    data = read_ube32 (dso, dso->shdr[i].sh_addr);
	    /* .got[0] points to _DYNAMIC, it needs to be adjusted.  */
	    if (data == dso->shdr[n].sh_addr && data >= start)
	      write_be32 (dso, dso->shdr[i].sh_addr, data + adjust);
	    break;
	  }
    }

  return 0;
}

static int
sparc_adjust_rel (DSO *dso, GElf_Rel *rel, GElf_Addr start,
		 GElf_Addr adjust)
{
  error (0, 0, "%s: Sparc doesn't support REL relocs", dso->filename);
  return 1;
}

static int
sparc_adjust_rela (DSO *dso, GElf_Rela *rela, GElf_Addr start,
		  GElf_Addr adjust)
{
  if (GELF_R_TYPE (rela->r_info) == R_SPARC_RELATIVE)
    {
      if (rela->r_addend)
	{
	  if (rela->r_addend >= start)
	    rela->r_addend += adjust;
	}
      else
	{
	  GElf_Addr val = read_ube32 (dso, rela->r_offset);

	  if (val >= start)
	    write_be32 (dso, rela->r_offset, val + adjust);
	}
    }
  return 0;
}

static int
sparc_prelink_rel (struct prelink_info *info, GElf_Rel *rel,
		   GElf_Addr reladdr)
{
  error (0, 0, "%s: Sparc doesn't support REL relocs", info->dso->filename);
  return 1;
}

static void
sparc_fixup_plt (DSO *dso, GElf_Rela *rela, GElf_Addr value)
{
  Elf32_Sword disp = value - rela->r_offset;

  if (disp >= -0x800000 && disp < 0x800000)
    {
      /* b,a value
	  nop
	 nop  */
      write_be32 (dso, rela->r_offset, 0x30800000 | ((disp >> 2) & 0x3fffff));
      write_be32 (dso, rela->r_offset + 4, 0x01000000);
      write_be32 (dso, rela->r_offset + 8, 0x01000000);
    }
  else
    {
      /* sethi %hi(value), %g1
	 jmpl %g1 + %lo(value), %g0
	  nop  */
      write_be32 (dso, rela->r_offset, 0x03000000 | ((value >> 10) & 0x3fffff));
      write_be32 (dso, rela->r_offset + 4, 0x81c06000 | (value & 0x3ff));
      write_be32 (dso, rela->r_offset + 8, 0x01000000);
    }
}

static int
sparc_prelink_rela (struct prelink_info *info, GElf_Rela *rela,
		    GElf_Addr relaaddr)
{
  DSO *dso = info->dso;
  GElf_Addr value;

  if (GELF_R_TYPE (rela->r_info) == R_SPARC_NONE)
    return 0;
  else if (GELF_R_TYPE (rela->r_info) == R_SPARC_RELATIVE)
    {
      /* 32-bit SPARC handles RELATIVE relocs as
	 *(int *)rela->r_offset += l_addr + rela->r_addend.
	 RELATIVE relocs against .got traditionally have the
	 addend in memory pointed by r_offset and 0 r_addend,
	 other RELATIVE relocs have 0 in memory and non-zero
	 r_addend.  For prelinking, we need the value in
	 memory to be already relocated for l_addr == 0 case,
	 so we have to make sure r_addend will be 0.  */
      if (rela->r_addend == 0)
	return 0;
      value = read_ube32 (dso, rela->r_offset);
      value += rela->r_addend;
      rela->r_addend = 0;
      write_be32 (dso, rela->r_offset, value);
      /* Tell prelink_rela routine it should update the relocation.  */
      return 2;
    }
  value = info->resolve (info, GELF_R_SYM (rela->r_info),
			 GELF_R_TYPE (rela->r_info));
  value += rela->r_addend;
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_SPARC_GLOB_DAT:
    case R_SPARC_32:
    case R_SPARC_UA32:
      write_be32 (dso, rela->r_offset, value);
      break;
    case R_SPARC_JMP_SLOT:
      sparc_fixup_plt (dso, rela, value);
      break;
    case R_SPARC_8:
      write_8 (dso, rela->r_offset, value);
      break;
    case R_SPARC_16:
    case R_SPARC_UA16:
      write_be16 (dso, rela->r_offset, value);
      break;
    case R_SPARC_LO10:
      write_be32 (dso, rela->r_offset,
		  (value & 0x3ff) | (read_ube32 (dso, rela->r_offset) & ~0x3ff));
      break;
    case R_SPARC_HI22:
      write_be32 (dso, rela->r_offset,
		  ((value >> 10) & 0x3fffff)
		  | (read_ube32 (dso, rela->r_offset) & 0xffc00000));
      break;
    case R_SPARC_DISP8:
      write_8 (dso, rela->r_offset, value - rela->r_offset);
      break;
    case R_SPARC_DISP16:
      write_be16 (dso, rela->r_offset, value - rela->r_offset);
      break;
    case R_SPARC_DISP32:
      write_be32 (dso, rela->r_offset, value - rela->r_offset);
      break;
    case R_SPARC_WDISP30:
      write_be32 (dso, rela->r_offset,
		  (((value - rela->r_offset) >> 2) & 0x3fffffff)
		  | (read_ube32 (dso, rela->r_offset) & 0xc0000000));
      break;
    case R_SPARC_COPY:
      if (dso->ehdr.e_type == ET_EXEC)
	/* COPY relocs are handled specially in generic code.  */
	return 0;
      error (0, 0, "%s: R_SPARC_COPY reloc in shared library?", dso->filename);
      return 1;
    default:
      error (0, 0, "%s: Unknown sparc relocation type %d", dso->filename,
	     (int) GELF_R_TYPE (rela->r_info));
      return 1;
    }
  return 0;
}

static int
sparc_apply_conflict_rela (struct prelink_info *info, GElf_Rela *rela,
			  char *buf)
{
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_SPARC_32:
    case R_SPARC_UA32:
      buf_write_be32 (buf, rela->r_addend);
      break;
    case R_SPARC_16:
    case R_SPARC_UA16:
      buf_write_be16 (buf, rela->r_addend);
      break;
    case R_SPARC_8:
      buf_write_8 (buf, rela->r_addend);
      break;
    default:
      abort ();
    }
  return 0;
}

static int
sparc_apply_rel (struct prelink_info *info, GElf_Rel *rel, char *buf)
{
  error (0, 0, "%s: Sparc doesn't support REL relocs", info->dso->filename);
  return 1;
}

static int
sparc_apply_rela (struct prelink_info *info, GElf_Rela *rela, char *buf)
{
  GElf_Addr value;

  value = info->resolve (info, GELF_R_SYM (rela->r_info),
			 GELF_R_TYPE (rela->r_info));
  value += rela->r_addend;
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_SPARC_NONE:
      break;
    case R_SPARC_DISP32:
      value -= rela->r_offset;
    case R_SPARC_GLOB_DAT:
    case R_SPARC_32:
    case R_SPARC_UA32:
      buf_write_be32 (buf, value);
      break;
    case R_SPARC_DISP16:
      value -= rela->r_offset;
    case R_SPARC_16:
    case R_SPARC_UA16:
      buf_write_be16 (buf, value);
      break;
    case R_SPARC_DISP8:
      value -= rela->r_offset;
    case R_SPARC_8:
      buf_write_8 (buf, value);
      break;
    case R_SPARC_LO10:
      buf_write_be32 (buf, (buf_read_ube32 (buf) & ~0x3ff) | (value & 0x3ff));
      break;
    case R_SPARC_HI22:
      buf_write_be32 (buf, (buf_read_ube32 (buf) & 0xffc00000)
			   | ((value >> 10) & 0x3fffff));
      break;
    case R_SPARC_WDISP30:
      buf_write_be32 (buf, (buf_read_ube32 (buf) & 0xc0000000)
			   | (((value - rela->r_offset) >> 2) & 0x3fffffff));
      break;
    case R_SPARC_RELATIVE:
      error (0, 0, "%s: R_SPARC_RELATIVE in ET_EXEC object?",
	     info->dso->filename);
      return 1;
    default:
      return 1;
    }
  return 0;
}

static int
sparc_prelink_conflict_rel (DSO *dso, struct prelink_info *info,
			    GElf_Rel *rel, GElf_Addr reladdr)
{
  error (0, 0, "%s: Sparc doesn't support REL relocs", dso->filename);
  return 1;
}

static int
sparc_prelink_conflict_rela (DSO *dso, struct prelink_info *info,
			     GElf_Rela *rela, GElf_Addr relaaddr)
{
  GElf_Addr value;
  struct prelink_conflict *conflict;
  GElf_Rela *ret;
  int r_type;

  if (GELF_R_TYPE (rela->r_info) == R_SPARC_RELATIVE
      || GELF_R_TYPE (rela->r_info) == R_SPARC_NONE)
    /* Fast path: nothing to do.  */
    return 0;
  conflict = prelink_conflict (info, GELF_R_SYM (rela->r_info),
			       GELF_R_TYPE (rela->r_info));
  if (conflict == NULL)
    return 0;
  value = conflict->lookupent->base + conflict->lookupval;
  ret = prelink_conflict_add_rela (info);
  if (ret == NULL)
    return 1;
  ret->r_offset = rela->r_offset;
  value += rela->r_addend;
  r_type = GELF_R_TYPE (rela->r_info);
  switch (r_type)    
    {
    case R_SPARC_DISP32:
      value -= rela->r_offset;
    case R_SPARC_GLOB_DAT:
    case R_SPARC_32:
      r_type = R_SPARC_32;
      break;
    case R_SPARC_DISP16:
      value -= rela->r_offset;
    case R_SPARC_16:
      r_type = R_SPARC_16;
      break;
    case R_SPARC_DISP8:
      value -= rela->r_offset;
    case R_SPARC_8:
      r_type = R_SPARC_8;
      break;
    /* Attempt to transform all reloc which read-modify-write into
       simple writes.  */
    case R_SPARC_LO10:
      value = (read_ube32 (dso, rela->r_offset) & ~0x3ff) | (value & 0x3ff);
      r_type = R_SPARC_32;
      break;
    case R_SPARC_HI22:
      value = (read_ube32 (dso, rela->r_offset) & 0xffc00000)
	      | ((value >> 10) & 0x3fffff);
      r_type = R_SPARC_32;
      break;
    case R_SPARC_WDISP30:
      value = (read_ube32 (dso, rela->r_offset) & 0xc0000000)
	      | (((value - rela->r_offset) >> 2) & 0x3fffffff);
      r_type = R_SPARC_32;
      break;
    case R_SPARC_UA16:
    case R_SPARC_UA32:
    case R_SPARC_JMP_SLOT:
      break;
    default:
      error (0, 0, "%s: Unknown Sparc relocation type %d", dso->filename,
	     r_type);
      return 1;
    }
  ret->r_info = GELF_R_INFO (0, r_type);
  ret->r_addend = value;
  return 0;
}

static int
sparc_rel_to_rela (DSO *dso, GElf_Rel *rel, GElf_Rela *rela)
{
  error (0, 0, "%s: Sparc doesn't support REL relocs", dso->filename);
  return 1;
}

static int
sparc_need_rel_to_rela (DSO *dso, int first, int last)
{
  return 0;
}

static int
sparc_arch_prelink (DSO *dso)
{
  return 0;
}

static int
sparc_reloc_size (int reloc_type)
{
  switch (reloc_type)
    {
    case R_SPARC_8:
    case R_SPARC_DISP8:
      return 1;
    case R_SPARC_16:
    case R_SPARC_DISP16:
    case R_SPARC_UA16:
      return 2;
    default:
      break;
    }
  return 4;
}

static int
sparc_reloc_class (int reloc_type)
{
  switch (reloc_type)
    {
    case R_SPARC_COPY: return RTYPE_CLASS_COPY;
    case R_SPARC_JMP_SLOT: return RTYPE_CLASS_PLT;
    default: return RTYPE_CLASS_VALID;
    }
}

PL_ARCH = {
  .class = ELFCLASS32,
  .machine = EM_SPARC,
  .alternate_machine = { EM_SPARC32PLUS },
  .R_JMP_SLOT = R_SPARC_JMP_SLOT,
  .R_COPY = R_SPARC_COPY,
  .R_RELATIVE = R_SPARC_RELATIVE,
  .adjust_dyn = sparc_adjust_dyn,
  .adjust_rel = sparc_adjust_rel,
  .adjust_rela = sparc_adjust_rela,
  .prelink_rel = sparc_prelink_rel,
  .prelink_rela = sparc_prelink_rela,
  .prelink_conflict_rel = sparc_prelink_conflict_rel,
  .prelink_conflict_rela = sparc_prelink_conflict_rela,
  .apply_conflict_rela = sparc_apply_conflict_rela,
  .apply_rel = sparc_apply_rel,
  .apply_rela = sparc_apply_rela,
  .rel_to_rela = sparc_rel_to_rela,
  .need_rel_to_rela = sparc_need_rel_to_rela,
  .reloc_size = sparc_reloc_size,
  .reloc_class = sparc_reloc_class,
  .max_reloc_size = 4,
  .arch_prelink = sparc_arch_prelink,
  /* Although TASK_UNMAPPED_BASE is 0x70000000, we leave some
     area so that mmap of /etc/ld.so.cache and ld.so's malloc
     does not take some library's VA slot.
     Also, if this guard area isn't too small, typically
     even dlopened libraries will get the slots they desire.  */
  .mmap_base = 0x71000000LL,
  .mmap_end =  0x80000000LL,
  .page_size = 0x10000
};
