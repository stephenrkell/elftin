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

#ifdef DYNAPPEND_AS_LIBRARY
#if 0 /* no header yet */
#include "dynappend.h"
#endif
#endif

/*
 Here we rewrite an ELF file so that
 a spare ELF dynamic section tag is instantiated with 
 */

static void usage(const char *basename)
{
	fprintf(stderr, "Usage: %s <filename> <tagnum> [tagval-as-decimal-number]\n", basename);
}
#ifdef DYNAPPEND_AS_LIBRARY
int dynappend(char *filename, char *tagnum_string, long *maybe_tagval)
{
#else
int main(int argc, char **argv)
{
	if (argc < 3)
	{
		usage(basename(argv[0]));
		return 1;
	}

	char *filename = argv[1];
	char *tagnum_string = argv[2];
	long tagval_if_needed;
	long *maybe_tagval = NULL;
	if (argc >= 4)
	{
		int ret = sscanf(argv[3], "%ld", &tagval_if_needed);
		if (ret > 0) maybe_tagval = &tagval_if_needed;
	}
#endif
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

	/* Okay, now do the dynappending.
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
	_Bool done_it = 0;
	for (Elf64_Shdr *shdr = shdrs; shdr < shdrs + ehdr->e_shnum; ++shdr)
	{
		if (shdr->sh_type == SHT_DYNAMIC)
		{
			const char *strtab = SECTION_DATA(shdrs[shdr->sh_link]);
			Elf64_Dyn *end = (Elf64_Dyn *) ((char*) SECTION_DATA(*shdr) + shdr->sh_size);
			for (Elf64_Dyn *d = SECTION_DATA(*shdr);
					(uintptr_t) d <= (uintptr_t) end;
					++d)
			{
				if (!d->d_tag && (uintptr_t)(d+1) <= (uintptr_t) end)
				{
					/* We've found our insertion point. */
					*d = (Elf64_Dyn) { .d_tag = atoi(tagnum_string) };
					if (maybe_tagval) d->d_un.d_val = *maybe_tagval;
					*(d+1) = (Elf64_Dyn) { .d_tag = DT_NULL };
					done_it = 1;
					break;
				}
			}
		}
	}

	munmap(mapping, length);
	close(fd);
	return !(done_it == 1);
}
