#ifndef ELFMAP_HH_
#define ELFMAP_HH_

#include <elf.h>
#include "relf.h"

/* Some C++ utilities for creating and navigating a memory mapping
 * of an ELF file. */

namespace elftin
{

struct fmap
{
	int fd;
	void *mapping;
	size_t mapping_size;
	off_t mapping_offset;
	off_t start_offset_from_mapping_offset;
	int mapping_err;
	fmap(int fd, size_t offset)
	{
		struct stat statbuf;
		int ret = fstat(fd, &statbuf);
		if (ret != 0) abort();
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
		this->fd = fd;
		this->mapping_offset = offset & (PAGE_SIZE - 1);
		this->start_offset_from_mapping_offset = offset - mapping_offset;
		this->mapping_size = ROUND_UP(statbuf.st_size - mapping_offset, PAGE_SIZE);
		this->mapping = mmap(NULL, mapping_size, PROT_READ, MAP_PRIVATE,
			fd, mapping_offset);
		if (mapping == MAP_FAILED)
		{
			this->mapping = nullptr;
			this->mapping_size = 0;
			this->mapping_err = errno;
		}
		else
		{
			this->mapping_err = 0;
		}
	}
	virtual ~fmap()
	{
		if (mapping) munmap(mapping, mapping_size);
	}
	off_t start_offset() const { return mapping_offset + start_offset_from_mapping_offset; }
	operator bool() const { return mapping != NULL; }
	operator void*() const { return (unsigned char *) mapping + start_offset_from_mapping_offset; }
	
	template<typename Target>
	Target *ptr(off_t o) const
	{
		return reinterpret_cast<Target*>((unsigned char *) mapping + start_offset_from_mapping_offset + o);
	}
	template<typename Target>
	Target& ref(off_t o) const
	{
		return *reinterpret_cast<Target*>((unsigned char *) mapping + start_offset_from_mapping_offset + o);
	}
};

struct elfmap : public fmap
{
	ElfW(Ehdr) *hdr;
private:
	void set_hdr()
	{
		if (this->mapping == MAP_FAILED) this->hdr = nullptr;
		else
		{
			assert(0 == memcmp(this->ptr<void>(0), "\x7f""ELF", 4));
			this->hdr = reinterpret_cast<ElfW(Ehdr) *>(this->operator void *());
		}
	}
public:
	elfmap(fmap&& to_upgrade) : fmap(std::move(to_upgrade))
	{ set_hdr(); }
	elfmap(int fd, size_t offset) : fmap(fd, offset)
	{ set_hdr(); }

	operator ElfW(Ehdr)*() const { return hdr; }

	template <ElfW(Word) sht>
	ElfW(Shdr)*
	find(ElfW(Shdr) *start = nullptr) const /* find first section header by SHT */
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
