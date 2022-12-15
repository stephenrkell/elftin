CXXFLAGS += -fPIC
CFLAGS += -fPIC
ifneq ($(LIBRUNT),)
CXXFLAGS += -I$(LIBRUNT)/include
endif
ifneq ($(LIBSRK31CXX),)
CXXFLAGS += -I$(LIBSRK31CXX)/include
endif
CXXFLAGS += -I../include/elftin/ldplugins

CXXFLAGS += -g

.PHONY: default
default: base-ldplugin.a

# we always build a .a and a .so
%.so: %.a
	$(CXX) -o $@ -shared $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -Wl,--whole-archive $< -Wl,--no-whole-archive $(LDLIBS)
BOOST_FILESYSTEM_LIB ?= -lboost_filesystem
base-ldplugin.a: base-ldplugin.o elfmap.o
	$(AR) r "$@" $+

.PHONY: clean
clean:
	rm -f *.o *.a *.so
