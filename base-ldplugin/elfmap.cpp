#include <cstdlib>
extern "C" {
#include <link.h>
}
#include <cstring>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cerrno>
#include <cassert>
#include <utility>

#include "elfmap.hh"
#include "base-ldplugin.hh" /* for debug_println */

namespace elftin
{
	fmap::~fmap()
	{
		if (mapping && should_unmap)
		{
			debug_println(1, "Finished with mapping at %p (size 0x%u)",
				mapping, (unsigned) mapping_size);
			munmap(mapping, mapping_size);
		}
	}
}
