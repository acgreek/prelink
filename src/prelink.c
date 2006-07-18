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
#include <endian.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "prelink.h"
#include "reloc.h"

static GElf_Addr
resolve_ldso (struct prelink_info *info, GElf_Word r_sym,
	      int reloc_type __attribute__((unused)))
{
  /* Dynamic linker does not depend on any other library,
     all symbols resolve to themselves with the exception
     of SHN_UNDEF symbols which resolve to 0.  */
  if (info->symtab[r_sym].st_shndx == SHN_UNDEF)
    {
      info->resolveent = NULL;
      return 0;
    }
  else
    {
      /* As the dynamic linker is relocated first,
	 l_addr will be 0.  */
      info->resolveent = info->ent;
      return 0 + info->symtab[r_sym].st_value;
    }
}

static GElf_Addr
resolve_dso (struct prelink_info *info, GElf_Word r_sym,
	     int reloc_type)
{
  struct prelink_symbol *s;
  int reloc_class = info->dso->arch->reloc_class (reloc_type);

  for (s = & info->symbols[r_sym]; s; s = s->next)
    if (s->reloc_class == reloc_class)
      break;

  if (s == NULL || s->ent == NULL)
    {
      info->resolveent = NULL;
      return 0;
    }

  info->resolveent = s->ent;
  return s->ent->base + s->value;
}

static int
prelink_rel (DSO *dso, int n, struct prelink_info *info)
{
  Elf_Data *data = NULL;
  Elf_Scn *scn = elf_getscn (dso->elf, n);
  GElf_Rel rel;
  int sec;

  while ((data = elf_getdata (scn, data)) != NULL)
    {
      int ndx, maxndx;
      GElf_Addr addr = dso->shdr[n].sh_addr + data->d_off;

      maxndx = data->d_size / dso->shdr[n].sh_entsize;
      for (ndx = 0; ndx < maxndx;
	   ++ndx, addr += dso->shdr[n].sh_entsize)
	{
	  gelfx_getrel (dso->elf, data, ndx, &rel);
	  sec = addr_to_sec (dso, rel.r_offset);
	  if (sec == -1)
	    continue;

	  switch (dso->arch->prelink_rel (info, &rel, addr))
	    {
	    case 2:
	      gelfx_update_rel (dso->elf, data, ndx, &rel);
	      break;
	    case 0:
	      break;
	    default:
	      return 1;
	    }
	}
    }
  return 0;
}

static int
prelink_rela (DSO *dso, int n, struct prelink_info *info)
{
  Elf_Data *data = NULL;
  Elf_Scn *scn = elf_getscn (dso->elf, n);
  GElf_Rela rela;
  int sec;

  while ((data = elf_getdata (scn, data)) != NULL)
    {
      int ndx, maxndx;
      GElf_Addr addr = dso->shdr[n].sh_addr + data->d_off;

      maxndx = data->d_size / dso->shdr[n].sh_entsize;
      for (ndx = 0; ndx < maxndx;
	   ++ndx, addr += dso->shdr[n].sh_entsize)
	{
	  gelfx_getrela (dso->elf, data, ndx, &rela);
	  sec = addr_to_sec (dso, rela.r_offset);
	  if (sec == -1)
	    continue;

	  switch (dso->arch->prelink_rela (info, &rela, addr))
	    {
	    case 2:
	      gelfx_update_rela (dso->elf, data, ndx, &rela);
	      break;
	    case 0:
	      break;
	    default:
	      return 1;
	    }
	}
    }
  return 0;
}

int
prelink_prepare (DSO *dso)
{
  struct reloc_info rinfo;
  int liblist = 0, libstr = 0, newlibstr = 0, undo = 0, newundo = 0;
  int i;
  
  for (i = 1; i < dso->ehdr.e_shnum; ++i)
    {
      const char *name
	= strptr (dso, dso->ehdr.e_shstrndx, dso->shdr[i].sh_name);
      if (! strcmp (name, ".gnu.liblist"))
	liblist = i;
      else if (! strcmp (name, ".gnu.libstr"))
	libstr = i;
      else if (! strcmp (name, ".gnu.prelink_undo"))
	undo = i;
      else if (! strcmp (name, ".mdebug")
	       && ((dso->arch->machine == EM_ALPHA
		    && dso->shdr[i].sh_type == SHT_ALPHA_DEBUG)
		   || (dso->arch->machine == EM_MIPS
		       && dso->shdr[i].sh_type == SHT_MIPS_DEBUG)))
	dso->mdebug_orig_offset = dso->shdr[i].sh_offset;
    }

  if (undo == 0)
    {
      Elf32_Shdr *shdr32;
      Elf64_Shdr *shdr64;

      dso->undo.d_size = gelf_fsize (dso->elf, ELF_T_SHDR,
				     dso->ehdr.e_shnum - 1, EV_CURRENT);
      dso->undo.d_buf = malloc (dso->undo.d_size);
      if (dso->undo.d_buf == 0)
	{
	  error (0, ENOMEM, "%s: Could not create .gnu.prelink_undo section",
		 dso->filename);
	  return 1;
	}
      dso->undo.d_type = ELF_T_SHDR;
      dso->undo.d_off = 0;
      dso->undo.d_align = gelf_fsize (dso->elf, ELF_T_ADDR, 1, EV_CURRENT);
      dso->undo.d_version = EV_CURRENT;
      switch (gelf_getclass (dso->elf))
	{
	case ELFCLASS32:
	  shdr32 = (Elf32_Shdr *) dso->undo.d_buf;
	  for (i = 1; i < dso->ehdr.e_shnum; ++i)
	    shdr32[i - 1] = *elf32_getshdr (elf_getscn (dso->elf, i));
	  break;
	case ELFCLASS64:
	  shdr64 = (Elf64_Shdr *) dso->undo.d_buf;
	  for (i = 1; i < dso->ehdr.e_shnum; ++i)
	    shdr64[i - 1] = *elf64_getshdr (elf_getscn (dso->elf, i));
	  break;
	default:
	  return 1;
	}
    }

  if (dso->ehdr.e_type != ET_DYN)
    return 0;

  if (is_ldso_soname (dso->soname))
    return 0;

  if (find_reloc_sections (dso, &rinfo))
    return 1;

  if (rinfo.reldyn && liblist && libstr && undo
      && ! rinfo.rel_to_rela && ! rinfo.rel_to_rela_plt)
      return 0;

  if (! liblist || ! libstr || ! undo || (rinfo.first && ! rinfo.reldyn))
    {
      Elf_Data data, *d;
      GElf_Shdr shdr;
      struct section_move *move;

      move = init_section_move (dso);
      if (move == NULL)
	return 1;

      if (rinfo.first && ! rinfo.reldyn)
	{
	  if (build_rel_dyn (dso, &data, &rinfo))
	    {
	      free (move);
	      return 1;
	    }

	  for (i = rinfo.last; i >= rinfo.first + 1; i--)
	    remove_section (move, i);
	  shdr = dso->shdr[rinfo.first];
	  shdr.sh_info = 0;
	  shdr.sh_size = data.d_size;
	}

      if (! liblist)
	{
	  liblist = move->old_to_new [dso->ehdr.e_shstrndx];
	  add_section (move, liblist);
	}
      else
	liblist = 0;

      if (! libstr)
	{
	  add_section (move, liblist + 1);
	  libstr = liblist + 1;
	  newlibstr = 1;
	}
      else
	libstr = move->old_to_new[libstr];

      if (! undo)
	{
	  add_section (move, libstr + 1);
	  undo = libstr + 1;
	  newundo = 1;
	}
      else
	undo = move->old_to_new[undo];

      if (reopen_dso (dso, move))
	{
	  free (data.d_buf);
	  free (move);
	  return 1;
	}

      free (move);
      if (rinfo.first && ! rinfo.reldyn)
	{
	  dso->shdr[rinfo.first] = shdr;
	  d = elf_getdata (elf_getscn (dso->elf, rinfo.first), NULL);
	  free (d->d_buf);
	  memcpy (d, &data, sizeof (data));
	  if (rinfo.plt)
	    rinfo.plt -= rinfo.last - rinfo.first;
	  rinfo.last = rinfo.first;
	  dso->shdr[rinfo.first].sh_name
	    = shstrtabadd (dso, data.d_type == ELF_T_REL
				? ".rel.dyn" : ".rela.dyn");
	  if (dso->shdr[rinfo.first].sh_name == 0)
	    return 1;
	}

      if (liblist)
	{
	  memset (&dso->shdr[liblist], 0, sizeof (GElf_Shdr));
	  dso->shdr[liblist].sh_name = shstrtabadd (dso, ".gnu.liblist");
	  if (dso->shdr[liblist].sh_name == 0)
	    return 1;
	  dso->shdr[liblist].sh_type = SHT_GNU_LIBLIST;
	  dso->shdr[liblist].sh_offset = dso->shdr[liblist - 1].sh_offset;
	  if (dso->shdr[liblist - 1].sh_type != SHT_NOBITS)
	    dso->shdr[liblist].sh_offset += dso->shdr[liblist - 1].sh_size;
	  dso->shdr[liblist].sh_link = libstr;
	  dso->shdr[liblist].sh_addralign = sizeof (GElf_Word);
	  dso->shdr[liblist].sh_entsize = sizeof (Elf32_Lib);
	}
      if (newlibstr)
        {
	  memset (&dso->shdr[libstr], 0, sizeof (GElf_Shdr));
	  dso->shdr[libstr].sh_name = shstrtabadd (dso, ".gnu.libstr");
	  if (dso->shdr[libstr].sh_name == 0)
	    return 1;
	  dso->shdr[libstr].sh_type = SHT_STRTAB;
	  dso->shdr[libstr].sh_offset = dso->shdr[libstr - 1].sh_offset;
	  if (dso->shdr[libstr - 1].sh_type != SHT_NOBITS)
	    dso->shdr[libstr].sh_offset += dso->shdr[libstr - 1].sh_size;
	  dso->shdr[libstr].sh_addralign = 1;
        }
      if (newundo)
	{
	  Elf_Scn *scn;
	  Elf_Data *data;
	  GElf_Addr newoffset;

	  memset (&dso->shdr[undo], 0, sizeof (GElf_Shdr));
	  dso->shdr[undo].sh_name = shstrtabadd (dso, ".gnu.prelink_undo");
	  if (dso->shdr[undo].sh_name == 0)
	    return 1;
	  dso->shdr[undo].sh_type = SHT_PROGBITS;
	  dso->shdr[undo].sh_offset = dso->shdr[undo - 1].sh_offset;
	  if (dso->shdr[undo - 1].sh_type != SHT_NOBITS)
	    dso->shdr[undo].sh_offset += dso->shdr[undo - 1].sh_size;
	  dso->shdr[undo].sh_addralign = dso->undo.d_align;
	  dso->shdr[undo].sh_entsize = gelf_fsize (dso->elf, ELF_T_SHDR, 1,
						   EV_CURRENT);
	  dso->shdr[undo].sh_size = dso->undo.d_size;
	  newoffset = dso->shdr[undo].sh_offset + dso->undo.d_align - 1;
	  newoffset &= ~(dso->shdr[undo].sh_addralign - 1);
	  if (adjust_dso_nonalloc (dso, undo + 1, dso->shdr[undo].sh_offset,
				   dso->undo.d_size + newoffset
				   - dso->shdr[undo].sh_offset))
	    return 1;
	  dso->shdr[undo].sh_offset = newoffset;
	  scn = elf_getscn (dso->elf, undo);
	  data = elf_getdata (scn, NULL);
	  assert (data != NULL && elf_getdata (scn, data) == NULL);
	  free (data->d_buf);
	  *data = dso->undo;
	  dso->undo.d_buf = NULL;
	}
    }
  else if (reopen_dso (dso, NULL))
    return 1;

  if (rinfo.rel_to_rela || rinfo.rel_to_rela_plt)
    {
      /* On REL architectures, we might need to convert some REL
	 relocations to RELA relocs.  */

      int safe = 1, align = 0;
      GElf_Addr start, adjust, adjust1, adjust2;

      for (i = 1; i < (rinfo.plt ? rinfo.plt : rinfo.first); i++)
	switch (dso->shdr[i].sh_type)
	  {
	  case SHT_HASH:
	  case SHT_DYNSYM:
	  case SHT_REL:
	  case SHT_RELA:
	  case SHT_STRTAB:
	  case SHT_NOTE:
	  case SHT_GNU_verdef:
	  case SHT_GNU_verneed:
	  case SHT_GNU_versym:
	    /* These sections are safe, no relocations should point
	       to it, therefore enlarging a section after sections
	       from this set only (and SHT_REL) in ET_DYN just needs
	       adjusting the rest of the library.  */
	    break;
	  default:
	    /* The rest of sections are not safe.  */
	    safe = 0;
	    break;
	  }

      if (! safe)
	{
	  error (0, 0, "%s: Cannot safely convert %s' section from REL to RELA",
		 dso->filename, strptr (dso, dso->ehdr.e_shstrndx,
					dso->shdr[rinfo.rel_to_rela
					? rinfo.first : rinfo.plt].sh_name));
	  return 1;
	}
                                                             
      for (i = rinfo.plt ? rinfo.plt : rinfo.first; i < dso->ehdr.e_shnum; i++)
	{
	  if (dso->shdr[i].sh_addralign > align)
	    align = dso->shdr[i].sh_addralign;
	}

      if (rinfo.plt)
	start = dso->shdr[rinfo.plt].sh_addr + dso->shdr[rinfo.plt].sh_size;
      else
	start = dso->shdr[rinfo.first].sh_addr + dso->shdr[rinfo.first].sh_size;

      adjust1 = 0;
      adjust2 = 0;
      assert (sizeof (Elf32_Rel) * 3 == sizeof (Elf32_Rela) * 2);
      assert (sizeof (Elf64_Rel) * 3 == sizeof (Elf64_Rela) * 2);
      if (rinfo.rel_to_rela)
	{
	  GElf_Addr size = dso->shdr[rinfo.first].sh_size / 2 * 3;
	  adjust1 = size - dso->shdr[rinfo.first].sh_size;
	  if (convert_rel_to_rela (dso, rinfo.first))
	    return 1;
	}
      if (rinfo.rel_to_rela_plt)
	{
	  GElf_Addr size = dso->shdr[rinfo.plt].sh_size / 2 * 3;
	  adjust2 = size - dso->shdr[rinfo.plt].sh_size;
	  if (convert_rel_to_rela (dso, rinfo.plt))
	    return 1;
	}

      adjust = adjust1 + adjust2;

      /* Need to make sure that all the remaining sections are properly
	 aligned.  */
      if (align)
	adjust = (adjust + align - 1) & ~(align - 1);

      /* Adjust all addresses pointing into remaining sections.  */
      if (adjust_dso (dso, start, adjust))
	return 1;

      if (rinfo.rel_to_rela)
	{
	  dso->shdr[rinfo.first].sh_size += adjust1;
	  if (rinfo.plt)
	    {
	      dso->shdr[rinfo.plt].sh_addr += adjust1;
	      dso->shdr[rinfo.plt].sh_offset += adjust1;
	    }
	}
      if (rinfo.rel_to_rela_plt)
	dso->shdr[rinfo.plt].sh_size += adjust2;

      if (update_dynamic_rel (dso, &rinfo))
	return 1;
    }

  if (rinfo.first && ! rinfo.reldyn && rinfo.relcount)
    set_dynamic (dso, dso->shdr[rinfo.first].sh_type == SHT_RELA
		      ? DT_RELACOUNT : DT_RELCOUNT, rinfo.relcount, 0);

  return 0;
}

static int
prelink_dso (struct prelink_info *info)
{
  int liblist = 0, libstr = 0;
  int i, ndeps = info->ent->ndepends + 1;
  DSO *dso = info->dso;
  Elf32_Lib *list = NULL;
  Elf_Scn *scn;
  Elf_Data *data;
  GElf_Addr oldsize, oldoffset;
  size_t strsize;

  if (ndeps <= 1 || dso->ehdr.e_type != ET_DYN)
    return 0;

  for (i = 1; i < dso->ehdr.e_shnum; ++i)
    {
      const char *name
	= strptr (dso, dso->ehdr.e_shstrndx, dso->shdr[i].sh_name);
      if (! strcmp (name, ".gnu.liblist"))
	liblist = i;
      else if (! strcmp (name, ".gnu.libstr"))
	libstr = i;
    }

  assert (liblist != 0);
  assert (libstr != 0);

  list = calloc (ndeps - 1, sizeof (Elf32_Lib));
  if (list == NULL)
    {
      error (0, ENOMEM, "%s: Cannot build .gnu.liblist section",
	     dso->filename);
      goto error_out;
    }

  strsize = 1;
  for (i = 0; i < ndeps - 1; ++i)
    {
      struct prelink_entry *ent = info->ent->depends[i];

      strsize += strlen (info->sonames[i + 1]) + 1;
      list[i].l_time_stamp = ent->timestamp;
      list[i].l_checksum = ent->checksum;
    }

  scn = elf_getscn (dso->elf, libstr);
  data = elf_getdata (scn, NULL);
  if (data == NULL)
    data = elf_newdata (scn);
  assert (elf_getdata (scn, data) == NULL);

  data->d_type = ELF_T_BYTE;
  data->d_size = 1;
  data->d_off = 0;
  data->d_align = 1;
  data->d_version = EV_CURRENT;
  data->d_buf = realloc (data->d_buf, strsize);
  if (data->d_buf == NULL)
    {
      error (0, ENOMEM, "%s: Could not build .gnu.libstr section",
	     dso->filename);
      goto error_out;
    }

  oldsize = dso->shdr[libstr].sh_size;
  dso->shdr[libstr].sh_size = 1;
  *(char *)data->d_buf = '\0';
  for (i = 0; i < ndeps - 1; ++i)
    {
      const char *name = info->sonames[i + 1];

      list[i].l_name = strtabfind (dso, liblist, name);
      if (list[i].l_name == 0)
	{
	  size_t len = strlen (name) + 1;

	  memcpy (data->d_buf + data->d_size, name, len);
	  list[i].l_name = data->d_size;
	  data->d_size += len;
	  dso->shdr[libstr].sh_size += len;
	}
    }
  if (oldsize != dso->shdr[libstr].sh_size)
    {
      GElf_Addr adjust = dso->shdr[libstr].sh_size - oldsize;

      oldoffset = dso->shdr[libstr].sh_offset;
      if (adjust_dso_nonalloc (dso, libstr + 1, oldoffset, adjust))
	goto error_out;
    }

  scn = elf_getscn (dso->elf, liblist);
  data = elf_getdata (scn, NULL);
  if (data == NULL)
    data = elf_newdata (scn);
  assert (elf_getdata (scn, data) == NULL);

  data->d_type = ELF_T_WORD;
  data->d_size = (ndeps - 1) * sizeof (Elf32_Lib);
  data->d_off = 0;
  data->d_align = sizeof (GElf_Word);
  data->d_version = EV_CURRENT;
  free (data->d_buf);
  data->d_buf = list;
  list = NULL;

  if (data->d_size != dso->shdr[liblist].sh_size)
    {
      GElf_Addr adjust = data->d_size - dso->shdr[liblist].sh_size;
      GElf_Addr newoffset;

      oldoffset = dso->shdr[liblist].sh_offset;
      newoffset = oldoffset;
      if (newoffset & (data->d_align - 1))
	{
	  newoffset = (newoffset + data->d_align - 1) & ~(data->d_align - 1);
	  adjust += newoffset - dso->shdr[liblist].sh_offset;
	}
      if (adjust_dso_nonalloc (dso, liblist + 1, oldoffset, adjust))
	goto error_out;
      dso->shdr[liblist].sh_offset = newoffset;
      dso->shdr[liblist].sh_size = data->d_size;
    }

  return 0;

error_out:
  free (list);
  return 1;
}

int
prelink_set_checksum (DSO *dso)
{
  extern uint32_t crc32 (uint32_t crc, unsigned char *buf, size_t len);
  uint32_t crc;
  int i, cvt;

  if (dynamic_info_is_set (dso, DT_GNU_PRELINKED_BIT)
      && set_dynamic (dso, DT_GNU_PRELINKED, 0, 1))
    return 1;

  if (set_dynamic (dso, DT_CHECKSUM, 0, 1))
    return 1;

  cvt = ! ((__BYTE_ORDER == __LITTLE_ENDIAN
	    && dso->ehdr.e_ident[EI_DATA] == ELFDATA2LSB)
	   || (__BYTE_ORDER == __BIG_ENDIAN
	       && dso->ehdr.e_ident[EI_DATA] == ELFDATA2MSB));
  crc = 0;
  for (i = 1; i < dso->ehdr.e_shnum; ++i)
    {
      if (! (dso->shdr[i].sh_flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR)))
	continue;
      if (dso->shdr[i].sh_type != SHT_NOBITS && dso->shdr[i].sh_size)
	{
	  Elf_Scn *scn = elf_getscn (dso->elf, i);
	  Elf_Data *d = NULL;

	  /* Cannot use elf_rawdata here, since the image is not written
	     yet.  */
	  while ((d = elf_getdata (scn, d)) != NULL)
	    {
	      if (cvt && d->d_type != ELF_T_BYTE)
		{
		  gelf_xlatetof (dso->elf, d, d,
				 dso->ehdr.e_ident[EI_DATA]);
		  crc = crc32 (crc, d->d_buf, d->d_size);
		  gelf_xlatetom (dso->elf, d, d,
				 dso->ehdr.e_ident[EI_DATA]);
		}
	      else
		crc = crc32 (crc, d->d_buf, d->d_size);
	    }
	}
    }

  if (set_dynamic (dso, DT_CHECKSUM, crc, 1))
    abort ();
  if (dso->info_DT_GNU_PRELINKED
      && set_dynamic (dso, DT_GNU_PRELINKED, dso->info_DT_GNU_PRELINKED, 1))
    abort ();
  dso->info_DT_CHECKSUM = crc;

  return 0;
}

static int
prelink_set_timestamp (struct prelink_info *info)
{
  DSO *dso = info->dso;

  info->ent->timestamp = (GElf_Word) time (NULL);
  dso->info_DT_GNU_PRELINKED = info->ent->timestamp;
  if (prelink_set_checksum (dso))
    return 1;
  info->ent->checksum = dso->info_DT_CHECKSUM;
  return 0;
}

static void
free_info (struct prelink_info *info)
{
  int i;

  free (info->symtab);
  free (info->dynbss);
  free (info->conflict_rela);
  if (info->conflicts)
    {
      for (i = 0; i < info->ent->ndepends + 1; ++i)
        {
	  struct prelink_conflict *c = info->conflicts[i];
	  void *f;

	  while (c != NULL)
	    {
	      f = c;
	      c = c->next;
	      free (f);
	    }
        }
      free (info->conflicts);
    }
  if (info->sonames)
    {
      for (i = 0; i < info->ent->ndepends + 1; ++i)
        free ((char *) info->sonames[i]);
      free (info->sonames);
    }
  if (info->symbols)
    {
      for (i = 0; i < info->symbol_count; ++i)
        {
          struct prelink_symbol *s = info->symbols[i].next;
          void *f;

	  while (s != NULL)
	    {
	      f = s;
	      s = s->next;
	      free (f);
	    }
        }
      free (info->symbols);
    }
}

int
prelink (DSO *dso, struct prelink_entry *ent)
{
  int i;
  Elf_Scn *scn;
  Elf_Data *data;
  struct prelink_info info;

  ent->pltgot = dso->info[DT_PLTGOT];

  if (! dso->info[DT_SYMTAB])
    return 0;

  if (! dso_is_rdwr (dso) && dso->ehdr.e_type == ET_DYN)
    {
      if (reopen_dso (dso, NULL))
	return 1;
    }
                        
  i = addr_to_sec (dso, dso->info[DT_SYMTAB]);
  /* DT_SYMTAB should be found and should point to
     start of .dynsym section.  */
  if (i == -1
      || dso->info[DT_SYMTAB] != dso->shdr[i].sh_addr)
    {
      error (0, 0, "%s: Bad symtab", dso->filename);
      return 1;
    }

  memset (&info, 0, sizeof (info));
  info.ent = ent;
  info.symtab_entsize = dso->shdr[i].sh_entsize;
  info.symtab = calloc (dso->shdr[i].sh_size / dso->shdr[i].sh_entsize,
			sizeof (GElf_Sym));
  if (info.symtab == NULL)
    {
      error (0, ENOMEM, "%s: Cannot convert .dynsym section", dso->filename);
      return 1;
    }

  scn = elf_getscn (dso->elf, i);
  data = NULL;
  while ((data = elf_getdata (scn, data)) != NULL)
    {
      int ndx, maxndx, loc;

      loc = data->d_off / info.symtab_entsize;
      maxndx = data->d_size / info.symtab_entsize;
      for (ndx = 0; ndx < maxndx; ++ndx)
	gelfx_getsym (dso->elf, data, ndx, info.symtab + loc + ndx);
    }
  info.symtab_start =
    adjust_new_to_old (dso, dso->shdr[i].sh_addr - dso->base);
  info.symtab_end = info.symtab_start + dso->shdr[i].sh_size;
  info.dso = dso;
  switch (prelink_get_relocations (&info))
    {
    case 0:
      goto error_out;
    case 1:
      info.resolve = resolve_ldso;
      break;
    case 2:
      info.resolve = resolve_dso;
      break;
    }

  if (dso->ehdr.e_type == ET_EXEC)
    {
      if (prelink_exec (&info))
	goto error_out;
    }
  else if (prelink_dso (&info))
    goto error_out;

  for (i = 1; i < dso->ehdr.e_shnum; i++)
    {
      if (! (dso->shdr[i].sh_flags & SHF_ALLOC))
	continue;
      if (! strcmp (strptr (dso, dso->ehdr.e_shstrndx,
			    dso->shdr[i].sh_name),
		    ".gnu.conflict"))
	continue;
      switch (dso->shdr[i].sh_type)
	{
	case SHT_REL:
	  if (prelink_rel (dso, i, &info))
	    goto error_out;
	  break;
	case SHT_RELA:
	  if (prelink_rela (dso, i, &info))
	    goto error_out;
	  break;
	}
    }

  if (dso->arch->arch_prelink && dso->arch->arch_prelink (dso))
    goto error_out;

  /* Must be last.  */
  if (dso->ehdr.e_type == ET_DYN
      && prelink_set_timestamp (&info))
    goto error_out;

  free_info (&info);
  return 0;

error_out:
  free_info (&info);
  return 1;
}
