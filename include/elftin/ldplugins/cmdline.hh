#ifndef LD_CMDLINE_HH_
#define LD_CMDLINE_HH_

#include <vector>
#include <string>
#include <set>
#include <map>
#include <utility>
#include <fcntl.h>
#include "elfmap.hh"
#include "base-ldplugin.hh" /* for debug_println */

namespace elftin
{

#define STARTS_WITH(var, conststr) \
(((var).substr(0, sizeof conststr - 1) == string(conststr)))
	/* GAH. We have to reimplement parsing of the linker command line.
	 * Is there really no way to defer this? I guess it's not too hard. Erk! */
using std::vector;
using std::string;
using std::pair;
using std::set;
using std::map;
using std::make_pair;
// FIXME: this is really lib code, not include code
inline vector<string> enumerate_input_files(vector<string> const& cmdline)
{
	vector<string> libpaths;
	vector<string> files;
	bool seen_dashdash = false;
	enum { DEFAULT, STATIC, DYNAMIC } bindmode = DEFAULT;
	// skip argv[0]
	for (auto i_opt = cmdline.begin() + 1; i_opt != cmdline.end(); ++i_opt)
	{
		string opt = *i_opt;
		if (STARTS_WITH(opt, "-")
			&& !seen_dashdash)
		{
			if (opt == "--") { seen_dashdash = true; continue; }
			set<string> set_binding_static_opts = { "-Bstatic", "-dn", "-non_shared", "-static" };
			set<string> set_binding_dynamic_opts = { "-Bdynamic", "-dy", "-call_shared" };
			if (set_binding_static_opts.find(opt) != set_binding_static_opts.end())
			{
				bindmode = STATIC;
			} else if (set_binding_dynamic_opts.find(opt) != set_binding_dynamic_opts.end())
			{
				bindmode = DYNAMIC;
			}
			const set<string> opts_with_arg = {
				"-a", "-A", "--architecture", "-b", "--format",
				"-c", "--mri-script", "--dependency-file",
				"-e", "--entry", "-f", "--auxiliary",
				"-F", "--filter", "-G", "--gpsize",
				"-h", "-soname", "-I", "--dynamic-linker",
				"-l", "--library", "-L", "--library-path",
				"-m", "-o", "--output", "--out-implib", "-plugin",
				"-plugin-opt", "-R", "--just-symbols",
				"--require-defined", "-y", "--trace-symbol", "-Y",
				"-assert", "--defsym", "-fini", "-init", "-Map",
				"--oformat", "--retain-symbols-file", "-rpath", "-rpath-link",
				"--sort-section", "--spare-dynamic-tags",
				"--task-link", "--section-start", "-Tbss", "-Tdata", "-Ttext",
				"-Ttext-segment", "-Trodata-segment", "-Tldata-segment",
				"--version-script", "--version-exports-section", "--dynamic-list",
				"--export-dynamic-symbol", "--export-dynamic-symbol-list",
				"--wrap", "--ignore-unresolved-symbol",
				"-z", "-P"
			};
			set<string>::const_iterator found = opts_with_arg.end();
			/* To extract the arg we may need to gobble equalses */
#define GETARG(iter, literal_opt) ( \
((iter)->length() == string(literal_opt).length()) ? \
((iter) + 1)->c_str() : \
({ const char *start = (iter)->c_str() + string(literal_opt).length(); \
   while (*start == '=') ++start; start; })\
)
			if (STARTS_WITH(opt, "-L"))
			{
				libpaths.push_back(GETARG(i_opt, "-L"));
			}
			// -l opts might really be a file
			bool really_a_file = false;
			if (STARTS_WITH(opt, "-l"))
			{
				string libname = GETARG(i_opt, "-l");
				/* Can we resolve this to a file? */
				for (auto i_libpath = libpaths.begin(); i_libpath != libpaths.end();
					++i_libpath)
				{
					vector<string> testpaths;
					// do we look for .so ? only if we're not -Bstatic
					if (bindmode != STATIC) testpaths.push_back(
						*i_libpath + "/lib" + libname + ".so");
					testpaths.push_back(
						*i_libpath + "/lib" + libname + ".a");
					for (auto i_testpath = testpaths.begin();
						i_testpath != testpaths.end();
						++i_testpath)
					{
						string testpath = *i_testpath;
						struct stat buf;
						int ret = stat(testpath.c_str(), &buf);
						if (ret == 0)
						{
							/* Need to check file type, not just existence?
							 * Though .so can also be a linker script, so maybe not.  */
							char *the_path = realpath(testpath.c_str(), NULL);
							if (!the_path) abort();
							// CARE: mutate opt and carry on as if file were named directly
							opt = string(the_path);
							free(the_path);
							really_a_file = true;
							break;
						}
					}
				}
			}
			/* We only have a discrete argument if they're not run together,
			 * e.g. "-Map=blah" vs "-Map blah" or "-b elf32-i386" vs "-belf32-i386" */
			if (opts_with_arg.end() != (found = opts_with_arg.find(opt))
				 && found->length() == opt.length())
			{
				// skip the next one
				++i_opt;
			}
			if (really_a_file) goto really_a_file;
			// HMM. This is horrible.
			// Can we use CLOEXEC and unlink()
			// to force cleanup? How do we tell apart
			// temporary files created elsewhere in the linker
			// from
			// random input (or output) files that happen to be under /tmp or TMPDIR?

			continue;
		}
		// if we got here, we have a non-option
	really_a_file:
		files.push_back(opt);
	}
	return files;
}

/* For each input object (not file), build a map
 * from that file to a function of that file, */
template <typename T>
map< pair<string, off_t> , T> classify_input_objects(vector<string> const& input_files,
	std::function< T(fmap const&, off_t, string const&) > interest)
{
	map< pair<string, off_t>, T > out;
	for (auto i_f = input_files.begin(); i_f != input_files.end(); ++i_f)
	{
		int fd = open(i_f->c_str(), O_RDONLY);
		if (fd == -1)
		{
			debug_println(0, "problem opening file `%s': %s", i_f->c_str(), strerror(errno));
			continue;
		}
		fmap f(fd, 0);
		if (f.mapping_size > 0 && 0 == memcmp(f.ptr<void>(0), "!<arch>\n", 8))
		{
			/* iterate over entries */
			static const char magic_bytes[] = { 0x60, 0x0a };
			struct ahdr
			{
				char name[16];  /* now at offset 16 */
				char timestamp_str[12]; /* now at offset 28 */
				char uid_str[6]; /* now at offset 34 */
				char gid_str[6]; /* now at offset 40 */
				char mode_str[8]; /* now at offset 48 */
				char size_str[10]; /* now at offset 58 */
				char magic[2]; /* now at offset 60 */
			};
			static_assert(sizeof (ahdr) == 60, "size of archive header");
			ahdr *hdr = nullptr;
			off_t offset = 8; // size of a global header
			for (; offset < f.mapping_size; offset += sizeof (ahdr) + atoi(hdr->size_str))
			{
				hdr = f.ptr<ahdr>(offset);
				if (0 != memcmp(hdr->magic, magic_bytes, sizeof magic_bytes))
				{
					// HMM. Not a valid archive?
					break;
				}
				out.insert(make_pair( make_pair(*i_f, offset + sizeof (ahdr)),
					interest(f, offset + sizeof (ahdr), *i_f + "(" + string(hdr->name) + ")") ));
			}
		} /* end archive case */
		else /* ELF file? linker script? we don't really care */
		{
			out.insert(make_pair( make_pair(*i_f, 0), interest(f, 0, *i_f) ));
		}
	close_and_continue:
		close(fd);
	}
	return out;
}

set< pair<ElfW(Sym)*, string> > enumerate_symbols_matching(fmap const& f, off_t offset,
	std::function<bool(ElfW(Sym)*, string const&)> pred)
{
	set< pair<ElfW(Sym)*, string> > matched;
	if (f.mapping_size > 0 && 0 == memcmp(f, "\x7f""ELF", 4))
	{
		debug_println(0, "We have an ELF at %p+0x%x", f.mapping, (unsigned) f.start_offset_from_mapping_offset);
		// it's already mapped! how do we do the 'upgrade'? need to point to it, unfortunately
		elfmap e(f);
		assert(e.mapping == f.mapping);
		if (e.hdr->e_ident[EI_CLASS] == ELFCLASS64 &&
			e.hdr->e_ident[EI_DATA] == ELFDATA2LSB &&
			e.hdr->e_type == ET_REL)
		{
			debug_println(0, "It's an interesting ELF");
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
#endif
