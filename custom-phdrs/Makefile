PHDR_NAME := meta
default: hello hello.with-$(PHDR_NAME)-phdr
include rules.mk

hello: LDLIBS += -Wl,-Map,hello.map -Wl,--verbose 2>&1 |\
	  LC_ALL=C\
          sed -e '/^=========/,/^=========/!d;/^=========/d'\
	  > hello.lds
hello: hello.c
	$(CC) -o "$@" $(CPPFLAGS) $(CFLAGS) "$<" $(LDFLAGS) $(LDLIBS)

hello.with-heap-phdr: LDLIBS += -Wl,-T,hello.with-heap-phdr.lds
hello.with-heap-phdr: hello.c hello.with-heap-phdr.lds
	$(CC) -o "$@" $(CPPFLAGS) $(CFLAGS) "$<" $(LDFLAGS) $(LDLIBS)

hello.with-meta-phdr: LDLIBS += -Wl,-T,hello.with-meta-phdr.lds
hello.with-meta-phdr: hello.c meta.o hello.with-meta-phdr.lds
	$(CC) -o "$@" $(CPPFLAGS) $(CFLAGS) "$<" meta.o $(LDFLAGS) $(LDLIBS)

meta.o: hello-meta.so
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 --rename-section .data=.meta "$<" "$@"
#--set-section-flags .data=noload "$<" "$@"

hello-meta.so: hello
	$(MAKE) -f ~/work/devel/liballocs.hg/tools/Makefile.allocsites /usr/lib/allocsites$(realpath $<)-types.so && \
		cp /usr/lib/allocsites$(realpath $<)-types.so "$@"

clean::
	rm -f hello meta.o
