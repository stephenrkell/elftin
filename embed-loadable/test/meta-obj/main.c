#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <assert.h>
#include <linux/auxvec.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>

#include "relf.h"

/* The ldso helper defines this for us. */
void *dlopen_from_fd(int fd, int flag);

char exename[4096];

int main(int argc, char **argv)
{
	/* Get an fd on ourselves */
	int fd = open(argv[0], O_RDONLY);
	if (fd == -1) return 1;
	int retval = 0;
	
	/* Grab the exe name from argv[0] */
	realpath(argv[0], exename);
	
	/* Look up our own phdrs and get the special one. */
	ElfW(auxv_t) *at_phdr = auxv_xlookup(get_auxv((const char **) environ, &fd), AT_PHDR);
	ElfW(Phdr) *phdr = (void*) at_phdr->a_un.a_val;
	off_t offset = 0;
	for (; phdr && phdr->p_type != PT_NULL; ++phdr)
	{
		if (phdr->p_type == 0x6ffffff0)
		{
			offset = phdr->p_offset;
			break;
		}
	}
	
	if (!offset) { retval = 2; goto out; }
	
	off_t ret = lseek(fd, offset, SEEK_SET);
	assert(ret != (off_t) -1);
	
	/* Use our dlopen_from_fd call */
	void *handle = dlopen_from_fd(fd, RTLD_NOW);
	assert(handle);
	fprintf(stderr, "got handle %p\n", handle);
	
out:
	close(fd);
	return retval;
}
