#define _GNU_SOURCE
#include <string.h>
#include <libgen.h>
#include <elf.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <assert.h>

/*
 Here we rewrite an ELF file so that for any relocation record,
 if it is using a section symbol but
 (1) a matching non-section symbol exists, and
 (2) certain heuristics pass (mostly: it's not a debugging section)
 we rewrite the reloc to use the non-section symbol.
 Optionally we also restrict our matching to a given symbol name.
 The name must match the **non-section symbol**.
 
 One thought: maybe we should be doing all this at the assembly
 level? That is where the localness of a symbol first gets disclosed,
 and it might be as simple as prepending '.globl <SYMNAME>' to the
 symbol.
 Indeed this seems to work, and it is the assembler that chooses
 whether to use the section or symbol reloc. But we have to detect
 that it's coming out as a local, and then re-run the assembler with
 munged assembly.  (Maybe? Could do inline sed script perhaps.)

 What happens if we have name conflicts? e.g. many similarly-structured
 modules all with address-taken allocator functions? Clearly we need to
 do a rename when we promote. The debugging information will still have
 the old source-level name, and that is exactly right.
 
 Thinking even higher-level: we could just do a source-level rewrite in
 CIL and get the right thing coming out. BUT that has several problems:
 - CIL is only for C
 - it does not preserve the source-level illusion in debugging information
 - our allocator specs should morally work at the ABI level, not the
    C-language level
 
 Indeed let's
 - do a toolsub-based asm wrapper when we're using toolsub generally
 - stick with this for now
 */

static void usage(const char *basename)
{
	fprintf(stderr, "Usage: %s <filename> [<sym>]\n", basename);
}
struct remembered_symbol {
	const char *name;
	Elf64_Sym *sym;
	Elf64_Shdr *shdr; // shdr for the symtab in which we found this
	struct remembered_symbol *associated;
};
int compare_remembered_sym_by_addr(const void *s1, const void *s2)
{
	// just compare by address of the Elf64_Sym
	return (int)(
		(intptr_t) ((struct remembered_symbol *) s1)->sym
			- (intptr_t) ((struct remembered_symbol *) s2)->sym
	);
}
int main(int argc, char **argv)
{
	if (argc < 2) // second arg is optional
	{
		usage(basename(argv[0]));
		return 1;
	}

	char *filename = argv[1];
	char *maybe_symname = argv[2];

	int fd = open(filename, O_RDWR);
	if (fd == -1)
	{
		warnx("could not open %s", filename);
		return 2;
	}

	struct stat buf;
	int ret = fstat(fd, &buf);

	long page_size = sysconf(_SC_PAGESIZE);

	if (ret)
	{
		warnx("could not stat %s", filename);
		return 3;
	}

	size_t length = (buf.st_size % page_size == 0) ? buf.st_size
				: page_size * (buf.st_size / page_size + 1);

	void *mapping = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
	{
		warnx("could not mmap %s", filename);
		return 4;
	}

	/* FIXME: don't assume 64-bit and native-endianness. */
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *) mapping;
	if (0 != strncmp(ehdr->e_ident, "\x7F""ELF", 4))
	{
		warnx("not an ELF file: %s", filename);
		return 5;
	}
#define INITIAL_LIST_SIZE 256
	unsigned zero_offset_list_size = 0;
	struct remembered_symbol *zero_offset_list = NULL;
	unsigned nzero_offset = 0;
	unsigned section_sym_list_size = 0;
	struct remembered_symbol *section_sym_list = NULL;
	unsigned nsection_sym = 0;
#define REALLOC_IF_FULL(fragment) do { \
	if (n ## fragment + 1 > fragment ## _list_size) \
	{ \
		fragment ## _list_size = (fragment ## _list_size) ? fragment ## _list_size * 2 : \
		     INITIAL_LIST_SIZE * sizeof (struct remembered_symbol); \
		fragment ## _list = realloc(fragment ## _list, fragment ## _list_size); \
		if (! fragment ## _list) err(1, "reallocating remembered list"); \
	} } while (0)
#define SECTION_DATA(shdr) ((void*)((uintptr_t) mapping + (shdr).sh_offset))
	Elf64_Shdr *shdrs = (Elf64_Shdr *) (ehdr->e_shoff ? (char*) mapping + ehdr->e_shoff : NULL);
	const char *shstrtab = SECTION_DATA(shdrs[ehdr->e_shstrndx]);
	// FIXME: we should have a better way of identifying debug sections
	// than by their name
#define IS_A_DEBUGGING_SECTION(shdr) \
    ((shdr)->sh_name && ( \
    0 == strncmp(&shstrtab[(shdr)->sh_name], ".debug_", sizeof ".debug_" - 1) || \
    0 == strncmp(&shstrtab[(shdr)->sh_name], ".eh_frame", sizeof ".eh_frame" - 1)))
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)
	{
		if (shdr->sh_type == SHT_SYMTAB)
		{
			const char *strtab = SECTION_DATA(shdrs[shdr->sh_link]);
			// 1. collect section symbols
			unsigned start_of_our_sections = nsection_sym;
			for (Elf64_Sym *sym = SECTION_DATA(*shdr);
					sym != (Elf64_Sym *) ((char*) SECTION_DATA(*shdr) + shdr->sh_size);
					++sym)
			{
				if (ELF64_ST_TYPE(sym->st_info) == STT_SECTION)
				{
					REALLOC_IF_FULL(section_sym);
					section_sym_list[nsection_sym++] = (struct remembered_symbol) {
						.name = sym->st_name ? &strtab[sym->st_name] : NULL,
						.sym = sym,
						.shdr = shdr
					};
				}
			}
			// 2. collect zero-offset symbols and associate with section syms
			for (Elf64_Sym *sym = SECTION_DATA(*shdr);
					sym != (Elf64_Sym *) ((char*) SECTION_DATA(*shdr) + shdr->sh_size);
					++sym)
			{
				const char *name = &strtab[sym->st_name];
				if (sym->st_name &&
					ELF64_ST_TYPE(sym->st_info) != STT_SECTION &&
					(!maybe_symname || 0 == strcmp(name, maybe_symname)) &&
					sym->st_shndx != SHN_UNDEF &&
					sym->st_shndx <= SHN_LORESERVE &&
					sym->st_value == 0)
				{
					REALLOC_IF_FULL(zero_offset);
					zero_offset_list[nzero_offset++] = (struct remembered_symbol) {
						.name = sym->st_name ? &strtab[sym->st_name] : NULL,
						.sym = sym,
						.shdr = shdr
					};
					// remember the association
					for (unsigned i = start_of_our_sections; i < nsection_sym; ++i)
					{
						if (sym->st_shndx == section_sym_list[i].sym->st_shndx)
						{
							if (!section_sym_list[i].associated)
							{
								section_sym_list[i].associated = &zero_offset_list[nzero_offset - 1];
							}
							else
							{
								warnx("Fishy: found multiple zero-offset replacements "
									"for section symbol of section `%s'",
									&shstrtab[shdrs[section_sym_list[i].sym->st_shndx].sh_name]);
							}
						}
					}
				}
			}
		}
	}
	/* qsort section_sym_list by literally the mmap'd addr of the Elf64_Sym.
	 * It has no incoming pointers.
	 * (NEVER sort zero_offset_list! It does have incoming pointers.) */
	qsort(section_sym_list, nsection_sym, sizeof (struct remembered_symbol),
		compare_remembered_sym_by_addr);
	/* Now we look for relocs referring to section syms from non-debug sections
	 * *or* to named ordinary syms from debug sections. Both need to be rewritten. */
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)
	{
		if (shdr->sh_type == SHT_REL || shdr->sh_type == SHT_RELA)
		{
			unsigned char *rels = SECTION_DATA(*shdr);
			// NOTE: this may be Rel or Rela and we cast on use
			Elf64_Shdr *symtab_shdr = &shdrs[shdr->sh_link];
			Elf64_Sym *symtab = SECTION_DATA(*symtab_shdr);
			char *strtab = SECTION_DATA(shdrs[symtab_shdr->sh_link]);
			Elf64_Shdr *relocated_sect_shdr = &shdrs[shdr->sh_info];
			void *relocated_sect_data = SECTION_DATA(*relocated_sect_shdr);
			// does this reloc reference a section symbol,
			// where that section has a corresponding zero-offset symbol, and
			// the relocation site matches our criteria, and
			// the zero-offset symbol matches our criteria?
			const unsigned sz = ((shdr->sh_type == SHT_REL) ?
					sizeof (Elf64_Rel) : sizeof (Elf64_Rela));
			for (unsigned char *rel = rels;
					rel != rels + shdr->sh_size;
					rel += sz)
			{
				/* Rel is a prefix of Rela, so just copy out the fields we want. */
				Elf64_Addr r_offset;
				memcpy(&r_offset, rel + offsetof(Elf64_Rel, r_offset), sizeof r_offset); 
				Elf64_Xword r_info;
				memcpy(&r_info, rel + offsetof(Elf64_Rel, r_info), sizeof r_info);
				Elf64_Sym *sym = symtab + ELF64_R_SYM(r_info);
				/* If the reference is coming from a debugging section, 
				 * we look for zero-offset-symbol relocs and turn them to section relocs.
				 */
				if (IS_A_DEBUGGING_SECTION(relocated_sect_shdr) &&
					(ELF64_ST_TYPE(sym->st_info) == STT_NOTYPE ||
						ELF64_ST_TYPE(sym->st_info) == STT_OBJECT ||
						ELF64_ST_TYPE(sym->st_info) == STT_FUNC ||
						ELF64_ST_TYPE(sym->st_info) == STT_COMMON) &&
						sym->st_value == 0)
					// FIXME: we don't require value==0! can use addends
				{
					warnx("found a from-debug reloc using ordinary symbol `%s'",
							&strtab[sym->st_name]);
					/* The reloc is using a zero-offset symbol, so let it
					 * use the section symbol instead. Sadly we have to linear-search
					 * for it. */
					int done_rewrite = 0;
					for (struct remembered_symbol *s = &section_sym_list[0];
							s < &section_sym_list[nsection_sym];
							++s)
					{
						if (s->sym->st_shndx == sym->st_shndx)
						{
							// s is a section symbol
							if (s->associated && !(sym == s->associated->sym))
							{
								warnx("reloc uses zero-offset sym that is not the associated one");
							}
							// do the rewrite
							Elf64_Xword new_r_info = ELF64_R_INFO(s->sym - symtab,
								ELF64_R_TYPE(r_info));
							memcpy(rel + offsetof(Elf64_Rel, r_info),
								&new_r_info,
								sizeof new_r_info);
							done_rewrite = 1;
							break;
						}
					}
					if (!done_rewrite)
					{
						warnx("did not rewrite a from-debug reloc using ordinary symbol `%s'",
							strtab[sym->st_name]);
					}
				}
				else if (!IS_A_DEBUGGING_SECTION(relocated_sect_shdr) &&
					ELF64_ST_TYPE(sym->st_info) == STT_SECTION)
				{
					// does this reloc site match our criteria?
					// FIXME: here is where we catch goto labels etc
#define INTERNAL_SELF_REFERENCE(_shdr, _offs, _info) (0)
					if (INTERNAL_SELF_REFERENCE(relocated_sect_shdr, r_offset, r_info)) continue;
					// do we know a corresponding zero-offset non-section sym
					// ...*in the same symtab*?
					// Look up this section sym in section_sym.
					struct remembered_symbol key = {
						.sym = sym
					};
					struct remembered_symbol *found = bsearch(&key, section_sym_list,
						nsection_sym, sizeof (struct remembered_symbol),
						compare_remembered_sym_by_addr);
					if (found)
					{
						/* We don't do rewrites for intra-section references via section symbols,
						 * e.g. for addr-taking of goto labels etc. */
						if (sym->st_shndx == relocated_sect_shdr - shdrs)
						{
							continue;
						}
						if (!found->associated)
						{
							warnx("NOT rewriting a reloc (shdr %u offset %u) to point to zero-offset sym: no sym found",
							(unsigned)(shdr - shdrs), (unsigned)((rel - rels) / sz));
						}
						else
						{
							// do the rewrite
							warnx("Rewriting a reloc (shdr %u offset %u) to point to zero-offset sym %s",
								(unsigned)(shdr - shdrs), (unsigned)((rel - rels) / sz),
									found->associated->name);
							Elf64_Xword new_r_info = ELF64_R_INFO(found->associated->sym - symtab,
								ELF64_R_TYPE(r_info));
							memcpy(rel + offsetof(Elf64_Rel, r_info),
								&new_r_info,
								sizeof new_r_info);
						}
					}
				}
			}
		}
	}

	if (zero_offset_list) free(zero_offset_list);
	if (section_sym_list) free(section_sym_list);
	munmap(mapping, length);
	close(fd);
	return 0;
}
