#include "malloc_private.h"

#if defined(__powerpc64__)
#define ASM_COMMENT "#"
#elif defined(__aarch64__)
#define ASM_COMMENT "//"
#elif defined(__x86_64__)
#define ASM_COMMENT "#"
#else
#error "Did not recognise architecture".
#endif

struct free_chunk
  tail __attribute__((section(".heap.bss.02, \"aw\", @nobits" ASM_COMMENT),aligned(16))),
  space __attribute__((section(".heap.data.02"),aligned(16))) = { &tail, POOL_SIZE },
  head __attribute__((section(".heap.data.01"),aligned(16))) = { &space };

unsigned char pool[POOL_SIZE] __attribute__((section(".heap.bss.01, \"aw\", @nobits" ASM_COMMENT),aligned(16)));
