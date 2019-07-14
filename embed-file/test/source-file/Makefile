THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
SECTION_NAME := .filehdr
PHDR_NAME := src

default: test-withsrc.with-$(PHDR_NAME)-phdr

OBJ_NAME := test-withsrc
FILE_TO_EMBED = main.c
include $(dir $(THIS_MAKEFILE))/../../../embed-file/rules.mk

CFLAGS += -std=c11
CFLAGS += -g

test-withsrc: main.o
	$(CC) -o "$@" $(CPPFLAGS) $(CFLAGS) $+ $(LDFLAGS) $(LDLIBS)
	test -e "$@"

test-withsrc.with-src-phdr: main.o src.o test-withsrc.with-src-phdr.lds
	$(CC) -o "$@" $(CPPFLAGS) $(CFLAGS) "$<" src.o $(LDFLAGS) -Wl,-T,test-withsrc.with-src-phdr.lds $(LDLIBS)
