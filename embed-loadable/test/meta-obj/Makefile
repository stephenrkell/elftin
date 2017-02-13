THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
SECTION_NAME := .meta
PHDR_NAME := meta
FILE_TO_EMBED := libfoo-shifted.so

# NOTE: we build this file. But as of 2016/1/19, it won't run correctly without
# my hacked ld.so. I will rewrite ldso-helper.c to be slightly less fussy/brittle
# soon (although then you will need a working liballocs toolchain).
default: test-load.with-$(PHDR_NAME)-phdr

OBJ_NAME := test-load
include $(dir $(THIS_MAKEFILE))/../../../embed-loadable/rules.mk

ifeq ($(LIBALLOCS),)
$(warning Please grab the liballocs build tree and then set LIBALLOCS)
endif

CFLAGS += -I$(LIBALLOCS)/include
CFLAGS += -std=c11
CFLAGS += -g

test-load: main.o ldso-helper.o
	$(CC) -o "$@" $(CPPFLAGS) $(CFLAGS) $+ $(LDFLAGS) $(LDLIBS)
	test -e "$@"

libfoo.so: CFLAGS += -fPIC
# Since our linker script patch only aligns the .meta output section  
# to the common page size, build the meta object so its phdrs record
# only this alignment also. This avoids congruence-modulo-alignment
# errors at load time.
libfoo.so: LDFLAGS += -Wl,-z,max-page-size=4096
libfoo.so: libfoo.c
	$(CC) -shared -o "$@" $(CFLAGS) $(CPPFLAGS) "$<" $(LDFLAGS)

test-load.with-meta-phdr: main.o ldso-helper.o meta.o test-load.with-meta-phdr.lds
	$(CC) -o "$@" $(CPPFLAGS) $(CFLAGS) "$<" ldso-helper.o meta.o $(LDFLAGS) -Wl,-T,test-load.with-meta-phdr.lds $(LDLIBS)

clean::
	rm -f libfoo.so libfoo-shifted.so
