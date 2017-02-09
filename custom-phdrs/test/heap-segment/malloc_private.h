struct free_chunk
{
	/* To mean "logically deleted", set the low bit. */
	struct free_chunk *next;
	unsigned long size:31; /* size *not* including the free_chunk */
}; 

#ifndef POOL_SIZE
#define POOL_SIZE (1 * 1024)
#endif

#ifndef PAGE_SIZE
/* guess it */
#if defined(__powerpc64__)
#define PAGE_SIZE 65536
#elif defined(__aarch64__)
#define PAGE_SIZE 65536
/* 4k is also supported */
#elif defined(__x86_64__)
#define PAGE_SIZE 4096
#elif defined (__i386__)
#define PAGE_SIZE 4096
#else
#define PAGE_SIZE 65536
#endif
#endif

extern struct free_chunk tail, head;
extern unsigned char pool[];
