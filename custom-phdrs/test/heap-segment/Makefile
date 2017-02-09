THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ELFTIN := $(realpath $(dir $(THIS_MAKEFILE))/../../..)
NATIVE_ARCH := $(shell uname -m)

# We use redirection-to-$@, so make sure partial/failed outputs are deleted.
.DELETE_ON_ERROR: 

default: hello.with-heap-phdr

OBJ_NAME := hello
PHDR_NAME := heap

# specify the input objects for our output binary
hello: hello.o malloc.o pool.o

hello.with-heap-phdr: hello.o malloc.o pool.o hello.with-$(PHDR_NAME)-phdr.lds
	$(CC) -o "$@" $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LDLIBS) $(filter-out %.lds,$+) \
	  -Wl,-T,$(filter %.lds,$+)

include $(ELFTIN)/custom-phdrs/rules.mk

# tell make where to find the source files
vpath %.c $(ELFTIN)/custom-phdrs
vpath %.lds $(ELFTIN)/custom-phdrs
vpath %.m4 $(ELFTIN)/custom-phdrs
vpath %.inc $(ELFTIN)/custom-phdrs
vpath %.patch $(ELFTIN)/custom-phdrs

CFLAGS += -std=c11 -g
LDFLAGS += -fuse-ld=bfd

clean::
	rm -f *.o *.lds *.lds.phdrify.m4
	rm -f *malloc.so
