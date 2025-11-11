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

/* Here we rewrite an ELF file's relocation section headers so that
 * they are just progbits.
 */

static void usage(const char *basename)
{
	fprintf(stderr, "Usage: %s <filename>\n", basename);
}
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
	Elf64_Shdr *shdrs = (Elf64_Shdr *) (((uintptr_t) mapping) + ehdr->e_shoff);
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)  // FIXME: respect entsz
	{
		if (shdr->sh_type == SHT_SYMTAB)
		{
			/* Let's walk the symbols and make sure any UNDs are given
			 * protected visibility. */
			Elf64_Sym *syms = (Elf64_Sym *) (((uintptr_t) mapping) + shdr->sh_offset);
			Elf64_Sym *syms_end = (Elf64_Sym *) (((uintptr_t) mapping) + shdr->sh_offset + shdr->sh_size);
			for (Elf64_Sym *sym = syms; sym != syms_end; ++sym) // FIXME: respect entsz
			{
				if (sym->st_shndx == SHN_UNDEF &&
					ELF64_ST_VISIBILITY(sym->st_other) != STV_HIDDEN &&
					ELF64_ST_VISIBILITY(sym->st_other) != STV_PROTECTED)
				{
					sym->st_other = ELF64_ST_VISIBILITY(STV_PROTECTED);
				}
			}
		}
	}
	munmap(mapping, length);
	close(fd);
}
