#ifndef ELFMAP_HH_
#define ELFMAP_HH_

#include <elf.h>
#include "relf.h"

/* Some C++ utilities for creating and navigating a memory mapping
 * of an ELF file. */

namespace elftin
{

struct elfmap
{
	void *mapping;
	size_t mapping_size;
	int mapping_err;
	ElfW(Ehdr) *hdr;
	elfmap(int fd, size_t offset)
	{
		struct stat statbuf;
		int ret = fstat(fd, &statbuf);
		if (ret != 0) abort();
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
		// FIXME: 'offset' is not being accounted for here
		this->mapping_size = ROUND_UP(statbuf.st_size, PAGE_SIZE);
		this->mapping = mmap(NULL, mapping_size, PROT_READ, MAP_PRIVATE,
			fd, offset);
		if (mapping == MAP_FAILED)
		{
			this->mapping = nullptr;
			this->mapping_size = 0;
			this->mapping_err = errno;
			this->hdr = nullptr;
		}
		else
		{
			this->mapping_err = 0;
			this->hdr = reinterpret_cast<ElfW(Ehdr) *>(this->mapping);
		}
	}
	~elfmap()
	{
		if (mapping) munmap(mapping, mapping_size);
	}
	
	operator bool() const { return mapping != NULL; }
	operator void*() const { return mapping; }
	operator ElfW(Ehdr)*() const { return reinterpret_cast<ElfW(Ehdr) *>(mapping); }

	template<typename Target>
	Target *ptr(ElfW(Off) o) const
	{
		return reinterpret_cast<Target*>((unsigned char *) mapping + o);
	}
	template<typename Target>
	Target& ref(ElfW(Off) o) const
	{
		return *reinterpret_cast<Target*>((unsigned char *) mapping + o);
	}

	template <ElfW(Word) sht>
	ElfW(Shdr)*
	find(ElfW(Shdr) *start = nullptr) const /* find first a section header by SHT */
	{
		ElfW(Shdr) *first = ptr<ElfW(Shdr)>(hdr->e_shoff);
		if (!start) start = first;
		for (auto i = start + 1; (i-first) < hdr->e_shnum; ++i)
		{
			if (i->sh_type == sht) return i;
		}
		return nullptr;
	}

};

} /* end namespace elftin */

#endif
