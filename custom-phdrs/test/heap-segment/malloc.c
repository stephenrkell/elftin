/*
	Basic idea: use a linked free list and a big lock.
	Both allocated and free chunks have a 16-byte header.
	All allocations are 16-byte-aligned.
	Use a first-fit allocation policy.
	Keep the list in what order? 
	Ordering by address allows coalescing to be non-ridiculously inefficient.
	Allocate from a statically sized segment that is mostly BSS (see pool.c).
	That's it!
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include "malloc_private.h"

#ifndef CAS
#define CAS(location, oldval, newval) \
	atomic_compare_exchange_strong(location, &oldval, newval)
#endif

#ifdef NO_SYSCALLS
#define fprintf(...) 0
#define write(...)   1
/* Make sure we can always abort -- just call address zero. */
void __private_abort(void) __attribute__((weak));
#define abort(...)   __private_abort()
#endif

#ifdef NO_SYSCALLS
#ifndef NO_TRACE
#define NO_TRACE
#endif
#ifndef NO_SANITY
#define NO_SANITY
#endif
#ifndef NO_DUMP
#define NO_DUMP
#endif
#endif

struct meta_info
{
	unsigned long size:31;
	unsigned long pad1:33;
	unsigned long pad2;
};

#define META_FOR_CHUNK(p) ((struct meta_info *) ((uintptr_t) (p) - sizeof (struct meta_info)))

static _Atomic(int) locked;

static void enter_cr(void)
{
	int oldval = 0;
	while (CAS(&locked, oldval, 1));
}

static void exit_cr(void)
{
	locked = 0;
}

static void print_state(void *not_expected, ptrdiff_t not_expected_dist, void *expected)
{
#ifndef NO_SANITY
#ifndef NO_DUMP
	fprintf(stderr, "head is at %p\n", &head);
#endif
	_Bool saw_expected = 0;
	for (struct free_chunk *p = head.next; p; p = p->next)
	{
		assert(!not_expected || 
				((char*) p - (char*) not_expected) < 0 || 
				((char*) p - (char*) not_expected) >= not_expected_dist);
		if (p == expected) saw_expected = 1;
#ifndef NO_DUMP
		fprintf(stderr, "free chunk of size %d at pool+0x%lx \n", 
			p->size, (char*) p - (char*) &pool[0]);
#endif
		assert(p->size <= POOL_SIZE);
		ptrdiff_t off = (char*) p - (char*) &pool[0];
		ptrdiff_t minsz = -(sizeof (struct free_chunk));
		assert(off >= minsz);
		assert(off <= POOL_SIZE);
	}
#ifndef NO_DUMP
	fprintf(stderr, "tail is at %p\n", &tail);
#endif
	assert(!expected || saw_expected);
#endif
}

static struct free_chunk *list_search(struct free_chunk *l, 
		_Bool (*cb)(struct free_chunk *n, void *arg),
		struct free_chunk **out_left_node,
		void *arg)
{
	struct free_chunk *prev = &head;
	struct free_chunk *cur = head.next;
	
	while (cur != &tail)
	{
		if (cb(cur, arg)) break;
		
		prev = cur;
		cur = cur->next;
	}
	
	*out_left_node = prev;
	return cur;
}

static struct free_chunk *list_insert(struct free_chunk *prev, struct free_chunk *next, 
	struct free_chunk *to_insert)
{
	prev->next = to_insert;
	to_insert->next = next;
}

static struct free_chunk *list_delete(struct free_chunk *prev, struct free_chunk *to_delete)
{
	prev->next = to_delete->next;
	to_delete->next = NULL;
}

static _Bool search_cb(struct free_chunk *test_chunk, void *p_sz)
{
	size_t required_sz = *(size_t *) p_sz;
	return required_sz <= test_chunk->size;
}

static _Bool address_ge(struct free_chunk *test_chunk, void *p_addr)
{
	return (char*) test_chunk >= (char*) p_addr;
}

static struct free_chunk *list_insert_in_address_order(struct free_chunk *to_insert)
{
	struct free_chunk *left = NULL, *right;
	right = list_search(&head, address_ge, &left, to_insert);
	/* we should always get a result */
	assert(right);
	assert(left != NULL);
	return list_insert(left, right, to_insert);
}

static _Bool pre_or_post_coalesce_cb(struct free_chunk *test_chunk, void *chunk_to_free)
{
	char *chunk_to_free_real_beginning = (char*) META_FOR_CHUNK(chunk_to_free);
	char *chunk_to_free_real_end = chunk_to_free_real_beginning
			+ META_FOR_CHUNK(chunk_to_free)->size;
	char *test_chunk_real_beginning = (char*) test_chunk;
	char *test_chunk_real_end = test_chunk_real_beginning
			+ sizeof (struct free_chunk) + test_chunk->size;
	_Bool pre_coalesce = test_chunk_real_end == chunk_to_free_real_beginning;
	_Bool post_coalesce = chunk_to_free_real_end == test_chunk_real_beginning;
	return pre_coalesce || post_coalesce;
	/* FIXME: handle the "coalesce both sides" case. */
}
static _Bool exact_match_cb(struct free_chunk *test_chunk, void *p_chunk)
{
	return (void*) test_chunk == p_chunk;
}

/* must already be in CR on entry to this function! */
static void *allocate_at(struct free_chunk *left_node, struct free_chunk *right_node, size_t size)
{
	list_delete(left_node, right_node);
	
	/* round up the asked-for size to a multiple of 16 */
	size = (size % 16 == 0) ? size : 16 * (1 + size / 16);
	
	size_t issued_node_size_without_header = right_node->size;
	void *chunk_to_issue = (unsigned char*) right_node + sizeof (struct meta_info);
	struct meta_info *meta = META_FOR_CHUNK(chunk_to_issue);
	*meta = (struct meta_info) {
		.size = size
	};
	
	/* is there space left in the free chunk? */
	struct free_chunk *reinserted = NULL;
	size_t total_allocated_size = size + sizeof (struct meta_info);
	if (issued_node_size_without_header + sizeof (struct free_chunk)
		 - total_allocated_size > sizeof (struct meta_info))
	{
		/* Okay, it's worth carving off a new chunk. */
		size_t new_chunk_size = issued_node_size_without_header + sizeof (struct free_chunk)
				 - total_allocated_size - sizeof (struct free_chunk);
		assert(new_chunk_size < POOL_SIZE);
		unsigned char *end_of_issued_chunk = (unsigned char *) chunk_to_issue + size;
		struct free_chunk *new_chunk = (struct free_chunk *) end_of_issued_chunk;
		*new_chunk = (struct free_chunk) {
			.size = new_chunk_size
		};
		reinserted = new_chunk;
		
		/* Now insert it into the free list, at the beginning */
		list_insert_in_address_order(new_chunk);
#ifndef NO_TRACE
		fprintf(stderr, "reinserted free portion starting at pool+%lx\n", (char*) new_chunk - (char*) &pool[0]);
#endif
	}
	
	exit_cr();

#ifndef NO_TRACE
	fprintf(stderr, "issuing chunk at pool+%lx\n", (char*) chunk_to_issue - (char*) &pool[0]);
#endif
	print_state(right_node, size + sizeof (struct meta_info), reinserted);
	return chunk_to_issue;
}

void *malloc(size_t size)
{
	/* To allocate a chunk, we first find a node that is big enough. 
	 * Then we split that node.
	 * If enough space is left, we insert what remains into the free list. */

	struct free_chunk *left_node;
	struct free_chunk *right_node = NULL;
	struct free_chunk *right_node_next;
	
#ifndef NO_TRACE
	fprintf(stderr, "got malloc(%lu); ", (unsigned long) size);
#endif
	print_state(NULL, 0, NULL);
	
	enter_cr();
	
	right_node = list_search(&head, search_cb, &left_node, &size);
	if (right_node == &tail)
	{
#ifndef NO_TRACE
		fprintf(stderr, "out of free space\n");
#endif
		exit_cr();
		return NULL;
	}
	/* Now right_node is a chunk that has been removed from the list. 
	 * Carve off any trailing space, and write its metadata.
	 * PROBLEM: alignment. Our meta_info is 16 bytes just to keep this simple.
	 * Ideally we would save space with a less regular arrangement, where only
	 * one word of meta info is used by allocated chunks, and free chunks always
	 * end at an 8-modulo-16 boundary. (Then coalescing becomes an issue.) */
	assert(right_node->size >= size);
	
	return allocate_at(left_node, right_node, size);
}

void *calloc(size_t size, size_t nmemb)
{
	void *alloc = malloc(size * nmemb);
	if (alloc) memset(alloc, 0, size * nmemb);
	return alloc;
}

void free(void *chunk)
{
	if (!chunk) return;
	struct free_chunk *free_chunk = (struct free_chunk *) META_FOR_CHUNK(chunk);
	struct free_chunk *orig_free_chunk = free_chunk;
	size_t size = META_FOR_CHUNK(chunk)->size;
	
	*free_chunk = (struct free_chunk) {
		.next = NULL,
		.size = size
	};
	
	enter_cr();
	
	/* Can we coalesce with an existing chunk? */
	struct free_chunk *left_node = NULL, *right_node;
	right_node = list_search(&head, pre_or_post_coalesce_cb, &left_node, chunk);
	if (right_node != &tail)
	{
		/* Are we pre- or post-coalescing? */
		_Bool existing_free_chunk_comes_after =
				(char*) META_FOR_CHUNK(chunk) + sizeof (struct meta_info)
				+ size
				== (char*) right_node;
		if (existing_free_chunk_comes_after)
		{
#ifndef NO_TRACE
			fprintf(stderr, "coalescing with following free chunk at pool+%lx\n", (char*) right_node - (char*) &pool[0]);
#endif
			free_chunk->size += (sizeof (struct free_chunk) + right_node->size);
			list_delete(left_node, right_node);
		}
		else // there's already a chunk before
		{
#ifndef NO_TRACE
			fprintf(stderr, "coalescing with preceding free chunk at pool+%lx\n", (char*) right_node - (char*) &pool[0]);
#endif
			right_node->size += (sizeof (struct meta_info) + size);
			// re-set free chunk so that we "expect" only this one in print_state
			free_chunk = right_node;
			// that's it!
			goto out;
		}
	}
	else
	{
		fprintf(stderr, "not coalescing\n");
	}
	/* Hook it into the free list. */
	list_insert_in_address_order(free_chunk);

out:
	exit_cr();
	
	fprintf(stderr, "freed chunk at pool+%lx\n", (char*) orig_free_chunk - (char*) &pool[0]);
	print_state(NULL, 0, free_chunk);
}

void *realloc(void *ptr, size_t size)
{
	if (!ptr) return malloc(size);
	
	struct meta_info *meta = META_FOR_CHUNK(ptr);
	if (meta->size >= size)
	{
		/* HACK: do nothing */
		return ptr;
	}
	else
	{
		void *newchunk = malloc(size);
		if (newchunk)
		{
			memcpy(newchunk, ptr, meta->size);
			free(ptr);
		}
		return newchunk;
	}
}

struct aligned_search_cb_args
{
	size_t size;
	size_t align;
	char *out_aligned_addr;
};
static _Bool aligned_search_cb(struct free_chunk *test_chunk, void *p_args_as_void)
{
	struct aligned_search_cb_args *p_args = (struct aligned_search_cb_args *) p_args_as_void;
	
	/* align up the test chunk start to the required alignment */
	uintptr_t test_chunk_start = (uintptr_t) test_chunk + sizeof (struct meta_info);
	char *aligned_up_start = (char*) ((test_chunk_start % p_args->align == 0)
			? test_chunk_start
			: p_args->align * (1 + test_chunk_start / p_args->align));
	size_t padding = aligned_up_start - (char*) test_chunk_start;
	if (test_chunk->size >= padding + p_args->size)
	{
		p_args->out_aligned_addr = aligned_up_start;
		return 1;
	}
	return 0;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	/* To allocate a chunk, we first find a node that is big enough. 
	 * Then we split that node.
	 * If enough space is left, we insert what remains into the free list. */

	if (alignment < 16) alignment = 16;
	
	struct free_chunk *left_node;
	struct free_chunk *right_node = NULL;
	struct free_chunk *right_node_next;
	
#ifndef NO_TRACE
	fprintf(stderr, "got posix_memalign(_, %lu, %lu); ", (unsigned long) alignment, 
		(unsigned long) size);
	print_state(NULL, 0, NULL);
#endif
	
	enter_cr();
	struct aligned_search_cb_args args = { size, alignment };
	right_node = list_search(&head, aligned_search_cb, &left_node, &args);
	if (right_node == &tail)
	{
#ifndef NO_TRACE
		fprintf(stderr, "out of free space\n");
#endif
		exit_cr();
		return ENOMEM;
	}
	/* Now right_node is a chunk that has been removed from the list. 
	 * Carve off any trailing space, and write its metadata.
	 * PROBLEM: alignment. Our meta_info is 16 bytes just to keep this simple.
	 * Ideally we would save space with a less regular arrangement, where only
	 * one word of meta info is used by allocated chunks, and free chunks always
	 * end at an 8-modulo-16 boundary. (Then coalescing becomes an issue.) */
	assert(right_node->size >= size);
	
	/* Split the right node at the aligned address;
	 * the original node gets its size decremented,
	 * and we insert a new node into the list. */
	if (args.out_aligned_addr != (char*) right_node + sizeof (struct meta_info))
	{
		char *split = args.out_aligned_addr - sizeof (struct meta_info);
		
		/* We should be splitting in such a way that issuing the chunk at "split"
		 * won't clobber the metadata of right_node. */
		assert(split - (char*) right_node >= 16);
		
#ifndef NO_TRACE
		fprintf(stderr, "splitting free chunk at pool+%lx, split at pool+%lx (%p)\n", (char*) right_node - (char*) &pool[0],
					split - (char*) &pool[0], split);
#endif

		struct free_chunk *new_node = (struct free_chunk *) split;
		*new_node = (struct free_chunk) {
			.next = NULL,
			.size = (((char*) right_node + sizeof (struct free_chunk) + right_node->size)
						- split) - sizeof (struct free_chunk)
		};
		right_node->size = ((char*) new_node - (char*) right_node) - sizeof (struct free_chunk);
#ifndef NO_TRACE
		fprintf(stderr, "first-portion free chunk at pool+%lx now has size %lu\n", 
				(char*) right_node - (char*) &pool[0], (unsigned long) right_node->size);
		fprintf(stderr, "second-portion free chunk at pool+%lx now has size %lu\n", 
				(char*) new_node - (char*) &pool[0], (unsigned long) new_node->size);
#endif
		list_insert_in_address_order(new_node);
		
		/* Now we have two free chunks next to each other (first right_node, then new_node). */
		left_node = right_node;
		right_node = new_node;
	}
	
	void *chunk = allocate_at(left_node, right_node, size);
	*memptr = chunk;
	return 0;
}

void *memalign(size_t alignment, size_t size)
{
	void *out = NULL;
	int ret = posix_memalign(&out, alignment, size);
	if (ret == 0) return out;
	return NULL;
}

void *aligned_alloc(size_t alignment, size_t size)
{
	return memalign(alignment, size);
}

void *valloc(size_t size)
{
	return memalign(PAGE_SIZE, size);
}

void *pvalloc(size_t size)
{
	return memalign(PAGE_SIZE, (size % PAGE_SIZE == 0) ? size : PAGE_SIZE * (1 + (size/PAGE_SIZE)));
}

size_t malloc_usable_size(void *ptr)
{
	return META_FOR_CHUNK(ptr)->size;
}

/* Define an __assert_fail() that doesn't malloc(). */
void __assert_fail(const char *assertion, const char *file, 
		unsigned int line, const char *function)
{
	int dummy __attribute__((unused)); /* used to silence warnings */
#define WRITE_STRING_LITERAL(s) \
	(dummy = write(2, (s), sizeof (s) - 1))
#define WRITE_STRING(s) \
	(dummy = write(2, (s), strlen((s))))
	WRITE_STRING_LITERAL("Assertion failed: `");
	WRITE_STRING(assertion);
	WRITE_STRING_LITERAL("' at ");
	WRITE_STRING(file);
	WRITE_STRING_LITERAL(":NN in "); // FIXME
	WRITE_STRING(function);
	abort();
}

/* Define simple memset() and memcpy(), as weaks, for if we need them. */
void *memset(void *s, int c, size_t n) __attribute__((weak));
void *memset(void *s, int c, size_t n)
{
	char *pos = s;
	while (n > 0)
	{
		*pos++ = c;
		--n;
	}
	return s;
}

void *memcpy(void *dest, const void *src, size_t n) __attribute__((weak));
void *memcpy(void *dest, const void *src, size_t n)
{
	const char *srcpos = src;
	char *destpos = dest;
	while (n > 0)
	{
		*destpos++ = *srcpos++;
		--n;
	}
	return dest;
}
