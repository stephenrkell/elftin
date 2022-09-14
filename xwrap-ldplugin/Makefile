CXXFLAGS := -fPIC
ifneq ($(LIBRUNT),)
CXXFLAGS += -I$(LIBRUNT)/include
endif

CXXFLAGS += -g

.PHONY: default
default: xwrap-ldplugin.so

%.so: %.cpp
	$(CXX) -o $@ -shared $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LDLIBS) $<

# HACK
/tmp/hello.c:
	/bin/echo -e '#include <stdio.h>\n\nint main(void)\n{\n\tprintf("Hello, world!\\n");\n\treturn 0;\n}\n' > $@

# use LD_DELAY_STARTUP to attach debugger
# use gold for now as it supports a wider API
.PHONY: test
test: xwrap-ldplugin.so /tmp/hello.c wrap-main.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDLIBS) -o /tmp/hello /tmp/hello.c \
    -fuse-ld=gold -Wl,-plugin -Wl,`pwd`/xwrap-ldplugin.so \
	-Wl,-plugin-opt=main wrap-main.o
# A known-failing test: WHY should this fail? For now: omit the wrapped code
.PHONY: testfail
testfail: xwrap-ldplugin.so /tmp/hello.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDLIBS) -o /dev/null /tmp/hello.c \
    -fuse-ld=gold -Wl,-plugin -Wl,`pwd`/xwrap-ldplugin.so \
	-Wl,-plugin-opt=main