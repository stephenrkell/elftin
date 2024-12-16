#ifndef ELFMAP_HH_
#define ELFMAP_HH_

#include <elf.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
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
	off_t mapping_offset;                    // the offset used for mmap()ping the file
	off_t start_offset_from_mapping_offset;  // offset from 'mapping' where the file data starts
	int mapping_err;
	bool should_unmap;
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
			this->should_unmap = false;
		}
		else
		{
			this->mapping_err = 0;
			this->should_unmap = true;
		}
	}
	virtual ~fmap();
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
	
	bool is_archive() const
	{ return mapping_size > 0 && 0 == memcmp(this->ptr<void>(0), "!<arch>\n", 8); }

	typedef std::array<unsigned char, EI_NIDENT> ident_array_t;
	typedef std::optional<ident_array_t> is_elf_file_ret_t;
	is_elf_file_ret_t is_elf_file() const
	{
		if (mapping_size > 0)
		{
			ident_array_t ident = { 0 };
			memcpy(&ident[0], this->ptr<void>(0), EI_NIDENT);
			if (0 == memcmp(&ident[0], "\x7f""ELF", 4))
			{
				return is_elf_file_ret_t(ident);
			}
		}
		return is_elf_file_ret_t();
	}
};

struct elfmap : public fmap
{
	/* FIXME: what if it's a 32-bit header and we are building on a 64-bit platform?
	 * We could take the 'gelf' approach and take a copy here. */
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
	/* Upgrade if we no longer need the fmap. */
	elfmap(fmap&& to_upgrade) : fmap(std::move(to_upgrade))
	{ set_hdr(); }
	/* Upgrade is no good if the original fmap needs to live on (e.g. in the caller);
	 * we support copying, but the original fmap owns the memory mapping. */
#if 0
	elfmap(fmap const& to_copy) : fmap(to_copy)
	{ set_hdr(); should_unmap = false; }
#endif
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
