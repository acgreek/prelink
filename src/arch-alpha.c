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
alpha_adjust_dyn (DSO *dso, int n, GElf_Dyn *dyn, GElf_Addr start,
		 GElf_Addr adjust)
{
  return 0;
}

static int
alpha_adjust_rel (DSO *dso, GElf_Rel *rel, GElf_Addr start,
		 GElf_Addr adjust)
{
  error (0, 0, "%s: Alpha doesn't support REL relocs", dso->filename);
  return 1;
}

static int
alpha_adjust_rela (DSO *dso, GElf_Rela *rela, GElf_Addr start,
		  GElf_Addr adjust)
{
  if (GELF_R_TYPE (rela->r_info) == R_ALPHA_RELATIVE
      || GELF_R_TYPE (rela->r_info) == R_ALPHA_JMP_SLOT)
    {
      GElf_Addr val = read_ule64 (dso, rela->r_offset);

      if (val >= start)
	{
	  write_le64 (dso, rela->r_offset, val + adjust);
	  if (val == rela->r_addend)
	    rela->r_addend += adjust;
	}
    }
  return 0;
}

static int
alpha_prelink_rel (struct prelink_info *info, GElf_Rel *rel,
		   GElf_Addr reladdr)
{
  error (0, 0, "%s: Alpha doesn't support REL relocs", info->dso->filename);
  return 1;
}

static void
alpha_fixup_plt (DSO *dso, GElf_Rela *rela, GElf_Addr relaaddr,
		 GElf_Addr value)
{
  Elf64_Sxword disp;
  Elf64_Addr plt;

  relaaddr -= dso->info[DT_JMPREL];
  relaaddr /= sizeof (Elf64_Rela);
  relaaddr *= 12;
  plt = dso->info[DT_PLTGOT] + 32 + relaaddr;
  disp = ((Elf64_Sxword) (value - plt - 12)) / 4;
  if (disp >= -0x100000 && disp < 0x100000)
    {
      int32_t hi, lo;

      hi = value - plt;
      lo = (int16_t) hi;
      hi = (hi - lo) >> 16;

      /* ldah $27,hi($27)
	 lda $27,lo($27)
	 br $31,value  */
      write_le32 (dso, plt, 0x277b0000 | (hi & 0xffff));
      write_le32 (dso, plt + 4, 0x237b0000 | (lo & 0xffff));
      write_le32 (dso, plt + 8, 0xc3e00000 | (disp & 0x1fffff));
    }
  else
    {
      int32_t hi, lo;

      hi = rela->r_offset - plt;
      lo = (int16_t) hi;
      hi = (hi - lo) >> 16;

      /* ldah $27,hi($27)
	 ldq $27,lo($27)
	 jmp $31,($27)  */
      write_le32 (dso, plt, 0x277b0000 | (hi & 0xffff));
      write_le32 (dso, plt + 4, 0xa77b0000 | (lo & 0xffff));
      write_le32 (dso, plt + 8, 0x6bfb0000);
    }
}

static int
alpha_is_indirect_plt (DSO *dso, GElf_Rela *rela, GElf_Addr relaaddr)
{
  Elf64_Addr pltaddr;
  uint32_t plt[3];
  int32_t hi, lo;

  relaaddr -= dso->info[DT_JMPREL];
  relaaddr /= sizeof (Elf64_Rela);
  relaaddr *= 12;
  pltaddr = dso->info[DT_PLTGOT] + 32 + relaaddr;
  hi = rela->r_offset - pltaddr;
  lo = (int16_t) hi;
  hi = (hi - lo) >> 16;
  plt[0] = read_ule32 (dso, pltaddr);
  plt[1] = read_ule32 (dso, pltaddr + 4);
  plt[2] = read_ule32 (dso, pltaddr + 8);
  if (plt[0] == (0x277b0000 | (hi & 0xffff))
      && plt[1] == (0xa77b0000 | (lo & 0xffff))
      && plt[2] == 0x6bfb0000)
    return 1;
  return 0;
}

static int
alpha_prelink_rela (struct prelink_info *info, GElf_Rela *rela,
		    GElf_Addr relaaddr)
{
  DSO *dso;
  GElf_Addr value;

  if (GELF_R_TYPE (rela->r_info) == R_ALPHA_RELATIVE
      || GELF_R_TYPE (rela->r_info) == R_ALPHA_NONE)
    /* Fast path: nothing to do.  */
    return 0;
  dso = info->dso;
  value = info->resolve (info, GELF_R_SYM (rela->r_info),
			 GELF_R_TYPE (rela->r_info));
  value += rela->r_addend;
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_ALPHA_GLOB_DAT:
    case R_ALPHA_REFQUAD:
      write_le64 (dso, rela->r_offset, value);
      break;
    case R_ALPHA_JMP_SLOT:
      write_le64 (dso, rela->r_offset, value);
      alpha_fixup_plt (dso, rela, relaaddr, value);
      break;
    default:
      error (0, 0, "%s: Unknown alpha relocation type %d", dso->filename,
	     (int) GELF_R_TYPE (rela->r_info));
      return 1;
    }
  return 0;
}

static int
alpha_apply_conflict_rela (struct prelink_info *info, GElf_Rela *rela,
			  char *buf)
{
  switch (GELF_R_TYPE (rela->r_info) & 0xff)
    {
    case R_ALPHA_GLOB_DAT:
    case R_ALPHA_REFQUAD:
    case R_ALPHA_JMP_SLOT:
      buf_write_le64 (buf, rela->r_addend);
      break;
    default:
      abort ();
    }
  return 0;
}

static int
alpha_apply_rel (struct prelink_info *info, GElf_Rel *rel, char *buf)
{
  error (0, 0, "%s: Alpha doesn't support REL relocs", info->dso->filename);
  return 1;
}

static int
alpha_apply_rela (struct prelink_info *info, GElf_Rela *rela, char *buf)
{
  GElf_Addr value;

  value = info->resolve (info, GELF_R_SYM (rela->r_info),
			 GELF_R_TYPE (rela->r_info));
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_ALPHA_NONE:
      break;
    case R_ALPHA_GLOB_DAT:
    case R_ALPHA_REFQUAD:
    case R_ALPHA_JMP_SLOT:
      buf_write_le64 (buf, value + rela->r_addend);
      break;
    case R_ALPHA_RELATIVE:
      error (0, 0, "%s: R_ALPHA_RELATIVE in ET_EXEC object?", info->dso->filename);
      return 1;
    default:
      return 1;
    }
  return 0;
}

static int
alpha_prelink_conflict_rel (DSO *dso, struct prelink_info *info,
			    GElf_Rel *rel, GElf_Addr reladdr)
{
  error (0, 0, "%s: Alpha doesn't support REL relocs", dso->filename);
  return 1;
}

static int
alpha_prelink_conflict_rela (DSO *dso, struct prelink_info *info,
			     GElf_Rela *rela, GElf_Addr relaaddr)
{
  GElf_Addr value;
  struct prelink_conflict *conflict;
  GElf_Rela *ret;

  if (GELF_R_TYPE (rela->r_info) == R_ALPHA_RELATIVE
      || GELF_R_TYPE (rela->r_info) == R_ALPHA_NONE)
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
  ret->r_info = GELF_R_INFO (0, GELF_R_TYPE (rela->r_info));
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_ALPHA_GLOB_DAT:
    case R_ALPHA_REFQUAD:
      ret->r_addend = value + rela->r_addend;
      break;
    case R_ALPHA_JMP_SLOT:
      ret->r_addend = value + rela->r_addend;
      if (alpha_is_indirect_plt (dso, rela, relaaddr))
	ret->r_info = GELF_R_INFO (0, R_ALPHA_GLOB_DAT);
      else
	{
	  relaaddr -= dso->info[DT_JMPREL];
	  relaaddr /= sizeof (Elf64_Rela);
	  if (relaaddr > 0xffffff)
	    {
	      error (0, 0, "%s: Cannot create R_ALPHA_JMP_SLOT conflict against .rel.plt with more than 16M entries",
		     dso->filename);
	      return 1;
	    }
	  ret->r_info = GELF_R_INFO (0, (relaaddr << 8) | R_ALPHA_JMP_SLOT);
	}
      break;
    default:
      error (0, 0, "%s: Unknown Alpha relocation type %d", dso->filename,
	     (int) GELF_R_TYPE (rela->r_info));
      return 1;
    }
  return 0;
}

static int
alpha_rel_to_rela (DSO *dso, GElf_Rel *rel, GElf_Rela *rela)
{
  error (0, 0, "%s: Alpha doesn't support REL relocs", dso->filename);
  return 1;
}

static int
alpha_need_rel_to_rela (DSO *dso, int first, int last)
{
  return 0;
}

static int
alpha_arch_prelink (DSO *dso)
{
  return 0;
}

static int
alpha_reloc_size (int reloc_type)
{
  return 8;
}

static int
alpha_reloc_class (int reloc_type)
{
  switch (reloc_type)
    {
    case R_ALPHA_JMP_SLOT: return RTYPE_CLASS_PLT;
    default: return RTYPE_CLASS_VALID;
    }
}

PL_ARCH = {
  .class = ELFCLASS64,
  .machine = EM_ALPHA,
  .alternate_machine = { EM_FAKE_ALPHA },
  .R_JMP_SLOT = R_ALPHA_JMP_SLOT,
  .R_COPY = -1,
  .R_RELATIVE = R_ALPHA_RELATIVE,
  .adjust_dyn = alpha_adjust_dyn,
  .adjust_rel = alpha_adjust_rel,
  .adjust_rela = alpha_adjust_rela,
  .prelink_rel = alpha_prelink_rel,
  .prelink_rela = alpha_prelink_rela,
  .prelink_conflict_rel = alpha_prelink_conflict_rel,
  .prelink_conflict_rela = alpha_prelink_conflict_rela,
  .apply_conflict_rela = alpha_apply_conflict_rela,
  .apply_rel = alpha_apply_rel,
  .apply_rela = alpha_apply_rela,
  .rel_to_rela = alpha_rel_to_rela,
  .need_rel_to_rela = alpha_need_rel_to_rela,
  .reloc_size = alpha_reloc_size,
  .reloc_class = alpha_reloc_class,
  .max_reloc_size = 8,
  .arch_prelink = alpha_arch_prelink,
  /* Although TASK_UNMAPPED_BASE is 0x0000020000000000, we leave some
     area so that mmap of /etc/ld.so.cache and ld.so's malloc
     does not take some library's VA slot.
     Also, if this guard area isn't too small, typically
     even dlopened libraries will get the slots they desire.  */
  .mmap_base = 0x0000020001000000LL,
  .mmap_end =  0x0000020100000000LL,
  .page_size = 0x10000
};
