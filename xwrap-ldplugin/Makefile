CXXFLAGS += -fPIC
CFLAGS += -fPIC
ifneq ($(LIBRUNT),)
CXXFLAGS += -I$(LIBRUNT)/include
endif
ifneq ($(LIBSRK31CXX),)
CXXFLAGS += -I$(LIBSRK31CXX)/include
endif
CXXFLAGS += -I../include/elftin/ldplugins
CXXFLAGS += -I../normrelocs
CFLAGS +=   -I../normrelocs
vpath %.c ../normrelocs

CXXFLAGS += -g

.PHONY: default
default: xwrap-ldplugin.so base-ldplugin.a

vpath %.a ../base-ldplugin

# we always build a .a and a .so
%.so: %.a
	$(CXX) -o $@ -shared $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -Wl,--whole-archive $< -Wl,--no-whole-archive $(LDLIBS)
BOOST_FILESYSTEM_LIB ?= -lboost_filesystem
xwrap-ldplugin.so: LDLIBS += -lbsd $(BOOST_FILESYSTEM_LIB) ../base-ldplugin/base-ldplugin.a -lffi
xwrap-ldplugin.a: normrelocs.o xwrap-ldplugin.o
	$(AR) r "$@" $+
normrelocs.o: CFLAGS += -DNORMRELOCS_AS_LIBRARY

# HACK
/tmp/hello.c:
	/bin/echo -e '#include <stdio.h>\n\nint main(void)\n{\n\tprintf("Hello, world!\\n");\n\treturn 0;\n}\n' > $@

# use LD_DELAY_STARTUP to attach debugger
# use gold for now as it supports a wider API
# boost filesystem 'invalid free' problem at shutdown: WHAT is this?
# could be coming from my use of ancient boost 1.55?
# Yes, upgrading to 1.67 seems to fix things.
.PHONY: test
test: xwrap-ldplugin.so /tmp/hello.c wrap-main.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDLIBS) -o /tmp/hello /tmp/hello.c \
    -fuse-ld=gold -Wl,-plugin -Wl,`pwd`/xwrap-ldplugin.so \
	-Wl,-plugin-opt=main wrap-main.o
	/tmp/hello | tee /dev/stderr | grep -q 'before'
# A known-failing test: WHY should this fail? For now: omit the wrapped code
.PHONY: testfail
testfail: xwrap-ldplugin.so /tmp/hello.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDLIBS) -o /dev/null /tmp/hello.c \
    -fuse-ld=gold -Wl,-plugin -Wl,`pwd`/xwrap-ldplugin.so \
	-Wl,-plugin-opt=main

.PHONY: clean
clean:
	rm -f *.o *.a *.so
