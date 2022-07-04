#define _GNU_SOURCE
#include <string.h>
#include <libgen.h>
#include <elf.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <search.h>
#include <assert.h>
#include <alloca.h>
#include <link.h> /* for ElfW */
#include "/home/stephen/work/devel/libdlbind.git/src/symhash.h" /* for GNU hash table building */

/* Here we rewrite an ELF file to resolve inconsistencies between
 * the .symtab and the .dynsym, in the .symtab's favour.
 *
 * It is useful to run after objcopy, which only modifies .symtab.
 *
 * For all dynsyms, if there is a name-matching sym in .symtab and
 * the dynsym's {definedness, vaddr, TODO: globalness, visibility?) does not match
 * the sym's. This catches redefinitions.
 *
 * To catch renames, we record symbols that are uniquely labelling their
 * defined address, and if we find a dynsym for which symtab has a differently-
 * -named symbol at that address, we update the name. FIXME: we don't currently
 * have a way to introduce new strings into dynstr, so this is rather limited.
 *
 * Let's use hsearch for our hash tables.
 *
 * This gets complicated because dynamic linking machinery (GOT and PLT entries,
 * symbol hash tables) may need to be regenerated. We know how to regenerate the
 * SysV hash table. We do not rename PLT entries, but probably we should.
 *
 * In the future, we want tools like this to be implemented via 'files as heaps'.
 * Handling the non-trivial redundancy between hash tables and the base data
 * will be a challenge, e.g. to capture in the editable asm output.
 */

static void usage(const char *basename)
{
	fprintf(stderr, "Usage: %s <filename>\n", basename);
}
char *strtab_find(const char *haystack, const char *haystack_end, const char *needle)
{
	const char *found;
	do {
		size_t n = strnlen(haystack, haystack_end - haystack);
		found = strstr(haystack, needle);
		if (found) return (char*) found;
		haystack = haystack + n + 1;
	} while (haystack < haystack_end);
	return NULL;
}

_Bool must_recompute_hash_tables;
int main(int argc, char **argv)
{
	if (argc != 2)
	{
		usage(basename(argv[0]));
		return 1;
	}

	char *filename = argv[1];
	int fd = open(filename, O_RDWR);
	if (fd == -1)
	{
		errx(2, "could not open %s", filename);
	}

	long page_size = sysconf(_SC_PAGESIZE);
	struct stat buf;
	int ret = fstat(fd, &buf);
	if (ret)
	{
		errx(3, "could not stat %s", filename);
	}

	size_t length = (buf.st_size % page_size == 0) ? buf.st_size
				: page_size * (buf.st_size / page_size + 1);

	void *mapping = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
	{
		errx(4, "could not mmap %s", filename);
	}
	/* FIXME: don't assume 64-bit and native-endianness. */
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *) mapping;
	if (0 != strncmp(ehdr->e_ident, "\x7F""ELF", 4))
	{
		errx(5, "not an ELF file: %s", filename);
	}
	/* First build a hash table of the symtab. */
#define MAX_SYMS 65535
#define SECTION_DATA(shdr) ((void*)((uintptr_t) mapping + (shdr).sh_offset))
	Elf64_Shdr *shdrs = (Elf64_Shdr *) (ehdr->e_shoff ? (char*) mapping + ehdr->e_shoff : NULL);
	const char *shstrtab = SECTION_DATA(shdrs[ehdr->e_shstrndx]);
	Elf64_Shdr *seen_symtab_shdr = NULL;
	struct hsearch_data syms_by_name = { 0 };
	ret = hcreate_r(MAX_SYMS, &syms_by_name);
	if (!ret) /* failed */ err(EXIT_FAILURE, "creating symbols hash table");
	struct hsearch_data sym_blacklist = { 0 };
	ret = hcreate_r(MAX_SYMS, &sym_blacklist);
	if (!ret) /* failed */ err(EXIT_FAILURE, "creating symbol blacklist hash table");
	struct hsearch_data syms_by_addr = { 0 };
	ret = hcreate_r(MAX_SYMS, &syms_by_addr);
	if (!ret) /* failed */ err(EXIT_FAILURE, "creating addresses hash table");
	struct hsearch_data addr_blacklist = { 0 };
	ret = hcreate_r(MAX_SYMS, &addr_blacklist);
	if (!ret) /* failed */ err(EXIT_FAILURE, "creating address blacklist hash table");
	char *strtab = NULL;
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)
	{
		if (shdr->sh_type == SHT_SYMTAB)
		{
			assert(!seen_symtab_shdr);
			seen_symtab_shdr = shdr;
			strtab = SECTION_DATA(shdrs[shdr->sh_link]);
			for (Elf64_Sym *sym = SECTION_DATA(*shdr);
					sym != (Elf64_Sym *) ((char*) SECTION_DATA(*shdr) + shdr->sh_size);
					++sym)
			{
				if (sym->st_name)
				{
					char *namestr = &strtab[sym->st_name];
					// enter it into the sym table, detecting if it was already there
					ENTRY k = (ENTRY) { .key = namestr, .data = sym };
					ENTRY *found = NULL;
					ACTION action = ENTER;
					ret = hsearch_r(k, action, &found, &syms_by_name);
					if (!ret) /* failed! */ err(EXIT_FAILURE, "adding to symbols hash table");
					assert(found);
					if (found->data != sym)
					{
						warnx("Found a duplicate symbol of name `%s'", namestr);
						/* Duplicate symbols will confuse us, so we keep a blacklist */
						ret = hsearch_r((ENTRY) { .key = namestr, .data = NULL },
							ENTER, &found, &sym_blacklist);
						if (!ret) /* failed! */ err(EXIT_FAILURE, "adding to symbol blacklist hash table");
					}
					// also enter it, by address, into the address table
#define ADDRBUF_SIZE (1 + (sizeof (long) * 2))
#define BUF_FOR_ADDR(v) ({ \
	char *addrbuf = alloca(ADDRBUF_SIZE); \
	assert(addrbuf); \
	snprintf(addrbuf, ADDRBUF_SIZE, "%lx", (long)(v)); \
	addrbuf; \
})
					char *addrbuf = BUF_FOR_ADDR(sym->st_value);
					found = NULL;
					action = ENTER;
					ret = hsearch_r((ENTRY) { .key = addrbuf, .data = sym },
						action, &found, &syms_by_addr);
					if (!ret) /* failed! */ err(EXIT_FAILURE, "adding to addresses hash table");
					assert(found);
					if (found->data != sym)
					{
						warnx("Found a duplicate symbol marking address 0x%lx (`%s' as well as `%s'",
							(long) sym->st_value,
							&strtab[((ElfW(Sym) *)found->data)->st_name], namestr);
						/* Duplicate addresses will confuse us, so we keep a blacklist */
						ret = hsearch_r((ENTRY) { .key = addrbuf, .data = NULL },
							ENTER, &found, &addr_blacklist);
						if (!ret) /* failed! */ err(EXIT_FAILURE, "adding to address blacklist hash table");
					}
				}
			}
		}
	}
	Elf64_Sym *dynsym_start = NULL;
	char *dynstr = NULL;
	char *dynstr_end = NULL;
	ElfW(Word) *sysv_hash = NULL;
	unsigned sysv_hash_sz;
	ElfW(Word) *gnu_hash = NULL;
	unsigned gnu_hash_sz;
	/* Now we're looking for a dynsym. */
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)
	{
		if (shdr->sh_type == SHT_GNU_HASH)
		{
			gnu_hash = SECTION_DATA(*shdr);
			gnu_hash_sz = shdr->sh_size;
			continue;
		}
		if (shdr->sh_type == SHT_HASH)
		{
			sysv_hash = SECTION_DATA(*shdr);
			sysv_hash_sz = shdr->sh_size;
			continue;
		}
		if (shdr->sh_type == SHT_DYNSYM)
		{
			dynstr = SECTION_DATA(shdrs[shdr->sh_link]);
			dynstr_end = dynstr + shdrs[shdr->sh_link].sh_size;
			ElfW(Sym) *dynsym;
			for (dynsym = dynsym_start = SECTION_DATA(*shdr);
					dynsym != (Elf64_Sym *) ((char*) SECTION_DATA(*shdr) + shdr->sh_size);
					++dynsym)
			{
				if (dynsym->st_name)
				{
					char *namestr = &dynstr[dynsym->st_name];
					/* Did we blacklist this name? */
#define IN(the_k, the_h) ({ \
	ENTRY tmp_k = (ENTRY) { .key = (the_k), .data = NULL }; \
	ENTRY *tmp_found = NULL; \
	ACTION tmp_action = FIND; \
	int ret = hsearch_r((tmp_k), tmp_action, &tmp_found, &(the_h)); \
	ret != 0; \
})
					if (IN(namestr, sym_blacklist)) /* found one */ continue;
					ENTRY *found = NULL;
					/* OK, now look in the symbols table. */
					ret = hsearch_r( (ENTRY) { .key = namestr, .data = NULL },
						FIND, &found, &syms_by_name);
					if (ret)
					{
						ElfW(Sym) *sym = found->data;
						/* Check consistency between this dynsym and the symtab entry. */
						// 1a. definedness
						if ((dynsym->st_shndx == SHN_UNDEF && sym->st_shndx != SHN_UNDEF)
						||  (sym->st_shndx == SHN_UNDEF && dynsym->st_shndx != SHN_UNDEF))
						{
							fprintf(stderr, "Different definedness, so patching: `%s'\n", namestr);
							dynsym->st_shndx = sym->st_shndx;
							dynsym->st_value = sym->st_value; // also copy value
							continue;
						}
						// 1b. absness
						if ((dynsym->st_shndx == SHN_ABS && sym->st_shndx != SHN_ABS)
						||  (sym->st_shndx == SHN_ABS && dynsym->st_shndx != SHN_ABS))
						{
							fprintf(stderr, "Different absness, so patching: `%s'\n", namestr);
							dynsym->st_shndx = sym->st_shndx;
							dynsym->st_value = sym->st_value;  // also copy value
							continue;
						}
						// 2. same name but different vaddr, i.e. symbol was redefined in symtab
						if (sym->st_value != dynsym->st_value)
						{
							fprintf(stderr, "Different vaddr, so patching: `%s'\n", namestr);
							dynsym->st_value = sym->st_value;
							continue;
						}
					} /* end if ret */
					/* Also look in the address table. If a different-named symbol is the unique
					 * marker of this address in the symtab, we assume it means that the symbol
					 * was renamed in the symtab. We want to do the equivalent renaming here.
					 *
					 * Interaction: what if we just patched that symbol's value i.e. we've already
					 * done a redefinition and now it's aliasing the address we're considering
					 * here? Well, we just did 'continue' so we won't take both paths. Maybe
					 * that's the best we can do. */
					/* Did we blacklist this name? */
					char *addrbuf = BUF_FOR_ADDR(dynsym->st_value);
					if (IN(addrbuf, addr_blacklist)) continue;
					ret = hsearch_r((ENTRY) { .key = addrbuf, .data = NULL }, FIND,
						&found, &syms_by_addr);
					char *found_name;
					if (ret && 0 != strcmp(found_name = &strtab[((ElfW(Sym) *)found->data)->st_name],
						namestr))
					{
						/* symtab has a unique and different name for this address.
						 * Rename the symbol to that name. PROBLEM: what if it's not
						 * in dynstr? */
						char *found = strtab_find(dynstr, dynstr_end, found_name);
						if (!found)
						{
							fprintf(stderr, "Can't rename `%s' to `%s' because the latter"
								" is not in .dynstr\n", namestr, found_name);
						}
						else
						{
							fprintf(stderr, "Renaming `%s' to `%s'\n", namestr, found);
							dynsym->st_name = found - &dynstr[0];
							must_recompute_hash_tables = 1;
						}
					}
				} /* end if name */
			} /* end for sym */
		} /* end if dynsym */
	} /* end for shdr */
	if (must_recompute_hash_tables)
	{
		if (gnu_hash) errx(99, "Unimplemented: rewriting GNU hash table");
		ElfW(Word) nbucket = sysv_hash[0];
		ElfW(Word) nchain = sysv_hash[1];
		ElfW(Word) (*buckets)[/*nbucket*/] = (ElfW(Word)(*)[]) &sysv_hash[2];
		ElfW(Word) (*chains)[/*nchain*/] = (ElfW(Word)(*)[]) &sysv_hash[2 + nbucket];
		bzero(sysv_hash, sysv_hash_sz);
		unsigned nsyms = nchain;
		elf64_hash_init((char*) sysv_hash, sysv_hash_sz, nbucket, nsyms, dynsym_start, dynstr);
	}
	hdestroy_r(&syms_by_name);
	hdestroy_r(&sym_blacklist);
	hdestroy_r(&syms_by_addr);
	hdestroy_r(&addr_blacklist);
	munmap(mapping, length);
	close(fd);
}
