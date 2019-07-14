#include <sys/auxv.h>
#include <link.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
	int retval = 0;
	/* Look up our own phdrs and get the special one. */
	ElfW(Phdr) *phdr = (void*) getauxval(AT_PHDR);
	off_t offset = 0;
	size_t sz = 0;
	for (; phdr && phdr->p_type != PT_NULL; ++phdr)
	{
		if (phdr->p_type == 0x6ffffff1)
		{
			offset = phdr->p_offset;
			sz = phdr->p_filesz;
			break;
		}
	}
	if (!offset) return 2;
	/* If our loader knew to LOAD these segments, we could read
	 * from memory. For now, read from the file. */
	int fd = open(argv[0], O_RDONLY);
	if (fd == -1) return 1;
	off_t ret = lseek(fd, offset, SEEK_SET);
	assert(ret != (off_t) -1);
	puts("My source code is as follows:\n");
	char buf[4096];
	ssize_t nread;
	do
	{
		nread = read(fd, buf, (sizeof buf > sz) ? sz : sizeof buf);
		write(1, buf, nread);
		sz -= nread;
	} while (nread > 0);
	fflush(stdout);
	
	close(fd);
out:
	return retval;
}
