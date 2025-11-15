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

/* Here we rewrite an ELF file that is a static PIE (ET_DYN)
 * into one that is ET_REL.
 * We:
 *
 * - in the ELF header, drop program headers and set e_type to ET_REL
 * - for each defined symbol in symtab, subtract its section load addr
 * - THEN for each section, delete the address
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
			/* Let's walk the symbols and make sure any non-UND non-ABSs are
			 * sectoin-relative. */
			Elf64_Sym *syms = (Elf64_Sym *) (((uintptr_t) mapping) + shdr->sh_offset);
			Elf64_Sym *syms_end = (Elf64_Sym *) (((uintptr_t) mapping) + shdr->sh_offset + shdr->sh_size);
			for (Elf64_Sym *sym = syms; sym != syms_end; ++sym) // FIXME: respect entsz
			{
				unsigned shn = sym->st_shndx;
				if (sym->st_shndx != SHN_UNDEF && sym->st_shndx <= SHN_LORESERVE)
				{
					assert(shn < ehdr->e_shnum);
					ElfW(Shdr) *shdr = &shdrs[shn];
					sym->st_value -= shdr->sh_addr;
				}
			}
		}
	}
	/* Now drop the section addresses. */
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)  // FIXME: respect entsz
	{
		if (shdr->sh_flags & SHF_ALLOC)
		{
			shdr->sh_addr = 0;
		}
	}
	/* Now doctor the ELF header */
	ehdr->e_type = ET_REL;
	ehdr->e_phoff = 0;
	ehdr->e_phentsize = 0;
	ehdr->e_phnum = 0;
	munmap(mapping, length);
	close(fd);
}
