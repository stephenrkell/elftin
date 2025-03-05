#include <memory.h>
#include <cassert>
extern "C" {
#include <link.h>
}
#include "cmdline.hh"

namespace elftin
{

set< pair<ElfW(Sym)*, string> > enumerate_symbols_matching(fmap const& f, off_t offset,
	std::function<bool(ElfW(Sym)*, string const&)> pred)
{
	set< pair<ElfW(Sym)*, string> > matched;
	if (f.mapping_size > 0 && 0 == memcmp(f, "\x7f""ELF", 4))
	{
		debug_println(1, "We have an ELF at %p+0x%x", f.mapping, (unsigned) f.start_offset_from_mapping_offset);
		// it's already mapped! how do we do the 'upgrade'? need to point to it, unfortunately
		elfmap e(f);
		assert(e.mapping == f.mapping);
		if (e.hdr->e_ident[EI_CLASS] == ELFCLASS64 &&
			e.hdr->e_ident[EI_DATA] == ELFDATA2LSB &&
			e.hdr->e_type == ET_REL)
		{
			debug_println(1, "It's an interesting ELF");
			/* Now we can use some of the linker functions. */
			/* Can we walk its symbols? */
			/* GAH. To get shdrs, need to do it ourselves. */
			const Elf64_Shdr *shdrs = e.ptr<ElfW(Shdr)>(e.hdr->e_shoff);
			/* Since we need to peek at the file contents to get
			 * headers and the like, maybe the get_input_section_count
			 * and get_input_section_contents calls are a bad idea.
			 * I notice that only ld.gold implements them; ld.bfd
			 * does not. */
			ElfW(Shdr) *found;
			if (nullptr != (found = e.find<SHT_SYMTAB>()))
			{
				ElfW(Sym) *symtab = e.ptr<ElfW(Sym>)(found->sh_offset);
				char *strtab = e.ptr<char>((shdrs + found->sh_link)->sh_offset);
				/* Walk the symtab */
				for (ElfW(Sym) *sym = symtab; (uintptr_t) sym < (uintptr_t) symtab + found->sh_size; ++sym)
				{
					const char *name = &strtab[sym->st_name];
					if (pred(sym, string(name)))
					{
						/* It defines a wrapped symbol */
						matched.insert(make_pair(sym, name));
					}
				}
			}
		}
	}
	return matched;
}

} /* end namespace elftin */
