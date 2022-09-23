#ifndef LD_CMDLINE_HH_
#define LD_CMDLINE_HH_

#include <vector>
#include <string>
#include <set>
#include <map>
#include <utility>
#include <fcntl.h>
#include "elfmap.hh"

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
// FIXME: this is really lib code, not include code
inline vector<string> enumerate_input_files(vector<string> const& cmdline)
{
	vector<string> libpaths;
	vector<string> files;
	bool seen_dashdash = false;
	enum { DEFAULT, STATIC, DYNAMIC } bindmode = DEFAULT;
	for (auto i_opt = cmdline.begin(); i_opt != cmdline.end(); ++i_opt)
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
#define GETARG(iter, literal_opt) ( \
((iter)->length() == string(literal_opt).length()) ? \
((iter) + 1)->c_str() : (iter)->c_str() + string(literal_opt).length() )
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

template <typename T>
map< pair<string, off_t> , T> classify_input_objects(vector<string> const& input_files,
	std::function< T(fmap const&, off_t) > interest)
{
	map< pair<string, off_t>, T > out;
	for (auto i_f = input_files.begin(); i_f != input_files.end(); ++i_f)
	{
		int fd = open(i_f->c_str(), O_RDONLY);
		if (fd == -1) continue;
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
				char magic[2]; /* now at offset 58 */
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
					interest(f, offset + sizeof (ahdr)) ));
			}
		} /* end archive case */
		else /* ELF file? linker script? we don't really care */
		{
			out.insert(make_pair( make_pair(*i_f, 0), interest(f, 0) ));
		}
	close_and_continue:
		close(fd);
	}
	return out;
}

} /* end namespace elftin */
#endif
