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

/*
 Here we rewrite an ELF file so that any ABS symbol of value 0
 whose name matches a section name
 is made relative to that section, rather than ABS.
 */

static void usage(const char *basename)
{
	fprintf(stderr, "Usage: %s <filename> [<sym>]\n", basename);
}
int main(int argc, char **argv)
{
	if (argc < 2) // second arg is optionl
	{
		usage(basename(argv[0]));
		return 1;
	}

	char *filename = argv[1];
	char *maybe_symbol = argv[2];

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

	/* Okay, now do the abs2unding.
	 * FIXME: don't assume 64-bit and native-endianness. */
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *) mapping;
	if (0 != strncmp(ehdr->e_ident, "\x7F""ELF", 4))
	{
		warnx("not an ELF file: %s", filename);
		return 5;
	}
#define SECTION_DATA(shdr) ((void*)((uintptr_t) mapping + (shdr).sh_offset))
	Elf64_Shdr *shdrs = (Elf64_Shdr *) (ehdr->e_shoff ? (char*) mapping + ehdr->e_shoff : NULL);
	const char *shstrtab = SECTION_DATA(shdrs[ehdr->e_shstrndx]);
	/* Do one pass where we collect the section names. */
	const char *section_names[ehdr->e_shnum];
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)
	{
		if (shdr->sh_name) section_names[shdr - shdrs] = &shstrtab[shdr->sh_name];
		else section_names[shdr - shdrs] = NULL;
	}
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)
	{
		if (shdr->sh_type == SHT_SYMTAB)
		{
			const char *strtab = SECTION_DATA(shdrs[shdr->sh_link]);
			for (Elf64_Sym *sym = SECTION_DATA(*shdr);
					sym != (Elf64_Sym *) ((char*) SECTION_DATA(*shdr) + shdr->sh_size);
					++sym)
			{
				if (sym->st_shndx == SHN_ABS && sym->st_value == 0)
				{
					/* So far so good, but is it in our list? */
					const char *name = &strtab[sym->st_name];
					for (unsigned i = 0; i < ehdr->e_shnum; ++i)
					{
						if (section_names[i] && 0 == strcmp(name, section_names[i]))
						{
							/* OK, make it point to that section */
							sym->st_shndx = i;
						}
					}
				}
			}
		}
	}

	munmap(mapping, length);
	close(fd);
}
