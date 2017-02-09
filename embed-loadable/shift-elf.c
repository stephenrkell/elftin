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
 Here we rewrite an ELF file so that all the file offsets
 are shifted by a fixed amount.
 
 The only structures which *should* contain file offsets
 are the ELF header, program headers and section headers.
 So the easiest way to implement this is a C program which
 uses mmap() to traverse and update those structures.
 */

static void usage(const char *basename)
{
	fprintf(stderr, "Usage: %s <filename> <offset>\n", basename);
}
int main(int argc, char **argv)
{
	if (argc < 3)
	{
		usage(basename(argv[0]));
		return 1;
	}
	
	char *filename = argv[1];
	char *offset_str = argv[2];
	int offset = atoi(offset_str);
	
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
	
	/* Okay, now do the shifting.
	 * FIXME: don't assume 64-bit and native-endianness. */
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *) mapping;
	if (0 != strncmp(ehdr->e_ident, "\x7F""ELF", 4))
	{
		warnx("not an ELF file: %s", filename);
		return 5;
	}
	Elf64_Shdr *shdrs = (Elf64_Shdr *) (ehdr->e_shoff ? (char*) mapping + ehdr->e_shoff : NULL);
	Elf64_Phdr *phdrs = (Elf64_Phdr *) (ehdr->e_phoff ? (char*) mapping + ehdr->e_phoff : NULL);
	if (ehdr->e_phoff) ehdr->e_phoff += offset;
	if (ehdr->e_shoff) ehdr->e_shoff += offset;
	
	for (int i = 0; i < ehdr->e_shnum; ++i)
	{
		shdrs[i].sh_offset += offset;
	}
	
	for (int i = 0; i < ehdr->e_phnum; ++i)
	{
		phdrs[i].p_offset += offset;
	}
	
	munmap(mapping, length);
	close(fd);
}
