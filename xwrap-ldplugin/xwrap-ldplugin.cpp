/* The xwrap plugin implements symbol wrapping
 * as described here:
 * https://www.humprog.org/%7Estephen/blog/2022/08/03#elf-symbol-wrapping-via-replacement
 *
 * ... being similar, but not identical, to the --wrap <symbol> option.
 * Like that, the input link job optionally contains __wrap_<sym> to which
 * references to <sym> are to be redirected, while references to __real_<sym>
 * will be bound to the original <sym>. Unlike --wrap, same-file references
 * are unbound, including self-references, and a pre-pass converts
 * meta-references into section-relative relocs so that they are correctly
 * skipped. An emitted DSO's <sym> will be the wrapper, playing correctly with
 * dynamic linking. This requires us to use the -z muldefs method (otherwise
 * we'd have to rename later, but the symbol hash table is hard to update).
 * If -z muldefs was not originally on the command line, we do a post-pass
 * to raise an error if multiple definitions are provided for any symbols
 * other than the wrapped ones.
 *
 * How do linker plugins work, and what subspace within that is how we want
 * ours to work? What I want is something like "rewrite rules over link jobs".
 * Given a declarative expression of the job, I want ways to tweak it. Since
 * plugins were mostly developed for LTO, which is a much deeper transformation
 * than I'm likely to need, a lot of the structure will be extraneous. But also,
 * some things I want won't be provided.
 *
 * Re-exec is in many ways a hack that arises when it's easier to do things on
 * the command line than via the plugin API. In what cases is re-exec *necessary*,
 * if any?
 *
 * Stages.
 * - 1. receive arguments (-plugin X -plugin-opt=Y), restarting with -z muldefs if necessary
 * - 2. pre-alias and normrelocs some stuff
 * - 3. pre-insert the linker script aliasing redefining X to equal __wrap_X
 * - 4. figure out a sane factoring -- start with a 'noop plugin' base class incl utility logic?
 */

#include <vector>
#include <set>
#include <map>
#include <string>
#include <regex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <libgen.h> /* for dirname() and GNU basename() -- must include before cstring */
#include <cstring>
#include <sys/mman.h>
#include <elf.h>
#include <cassert>
#include "plugin-api.h"
#include <unistd.h> /* for sleep() */
#include <boost/algorithm/string.hpp> /* for split, is_any_of */
#include <sys/types.h> /* for fstat() */
#include <sys/stat.h> /* for fstat() */
#include <unistd.h> /* for fstat(), write(), read() */
#include <utility> /* for pair */
#include <algorithm> /* for find_if and remove_if */
#include <boost/optional.hpp>
#include <boost/filesystem.hpp>
#include <bsd/string.h>
extern "C" {
#include <link.h>
}
#include <malloc.h> /* for malloc_usable_size */
#include "normrelocs.h"
#include "elfmap.hh"
#include "cmdline.hh"
#include "plugin-api.hh"
#include "restart-self.hh"
#include "base-ldplugin.hh"

using std::vector;
using std::string;
using std::set;
using std::map;
using std::smatch;
using std::regex;
using std::regex_match;
using std::pair;
using std::make_pair;
using boost::optional;

using namespace elftin;

struct xwrap_plugin : elftin::linker_plugin
{
	map< pair<string, off_t>, set<string> > xwrapped_defined_symnames_by_input_file;
	struct claimed_file
	{
		const struct ld_plugin_input_file *input_file;
		string name;
		vector<string> syms;

		claimed_file(const std::pair<const struct ld_plugin_input_file *, string> & p)
			: input_file(p.first), name(p.second) {}
	};
	vector< claimed_file > claimed_files;
	vector< const struct ld_plugin_input_file * > input_files;
	/* The plugin library's "claim file" handler.  */
	enum ld_plugin_status
	claim_file(const struct ld_plugin_input_file *file, int *claimed)
	{
		debug_println(0, "claim-file handler called (on `%s', currently %d)", file->name, *claimed);
		/* If we "claim" a file, we are responsible for feeding its contents
		 * to the linker. How is this done in, say, the LLVM LTO plugin?
		 * In the claim-file hook, it just claims files and grabs input data.
		 * In the all-symbols-read hook, it creates lots of temporary files
		 * and does codegen. See below (in all_symbols_read_hook) for more on that.
		 */
		/* Things we can do in here: 

			(*get_input_section_count) (const void* handle, unsigned int *count);
			(*get_input_section_type) (const struct ld_plugin_section section,
												 unsigned int *type);
			(*get_input_section_name) (const struct ld_plugin_section section,
												 char **section_name_ptr);
			(*get_input_section_contents) (const struct ld_plugin_section section,
													 const unsigned char **section_contents,
													 size_t* len);
			(*update_section_order) (const struct ld_plugin_section *section_list,
							   unsigned int num_sections);
			(*allow_section_ordering) (void);
			(*allow_unique_segment_for_sections) (void);
			(*unique_segment_for_sections) (
				const char* segment_name,
				uint64_t segment_flags,
				uint64_t segment_alignment,
				const struct ld_plugin_section * section_list,
				unsigned int num_sections);
			(*get_input_section_alignment) (const struct ld_plugin_section section,
													  unsigned int *addralign);
			(*get_input_section_size) (const struct ld_plugin_section section,
												 uint64_t *secsize);

			... and possibly some others.
		*/
		/* How can we get the number of symbols? 
		 * We can't.
		 * As far as the linker is concerned, if we claim the file,
		 * there are no symbols except the ones we tell it about;
		 * it's our job to feed the linker symbols (now) and (later)
		 * sections!
		 * 
		 * Oh. But wait. What about the get_input_section_contents stuff?
		 * It sounds like it can walk the sections for us, just not
		 * the symbols. That's a bit odd. I suppose it allows ELF-packaging
		 * of other-format stuff, including intermediate symbol tables.
		 * So try: test whether it's a relocatable file, and if so, use
		 * the section calls to find the symtab.
		 */
		auto found = xwrapped_defined_symnames_by_input_file.find(make_pair(file->name, file->offset));
		if (found != xwrapped_defined_symnames_by_input_file.end()
			&& found->second.size() > 0)
		{
			*claimed = 1;
			/* Make a temp that will stand in for this file. */
			auto tmpfile = new_temp_file("xwrap-ldplugin");
			string tmpname = tmpfile.first;
			int tmpfd = tmpfile.second;
			if (tmpfd == -1) abort();
			debug_println(0, "Claimed file is replaced by temporary %s", tmpname.c_str());
			claimed_files.push_back(make_pair(file, tmpname));
			for (auto i_sym = found->second.begin(); i_sym != found->second.end(); ++i_sym)
			{
				claimed_files.back().syms.push_back(*i_sym);
			}
			/* We used to run the following script to generate a replacement file.

	echo "$0" "$@" 1>&2
	tmpname="$1"
	shift
	origname="$1"
	shift
	# now we have symnames in $@

	NORMRELOCS="${NORMRELOCS:-`dirname "$0"`/../normrelocs/normrelocs}"

	cat "$origname" > "$tmpname"
	for sym in $@; do
    	${NORMRELOCS} "$tmpname" "$sym" || exit 1
	done
	newtmp=`mktemp --suffix=.o`
	ld -r `for sym in $@; do echo --defsym __real_$sym=$sym; done` -o "$newtmp" "$tmpname" && \
	mv "$newtmp" "$tmpname"

			 */
			// copy origname to tmpname
			boost::filesystem::path origname_p(string(claimed_files.back().input_file->name));
			boost::filesystem::path tmpname_p(tmpname);
			boost::filesystem::copy_file(origname_p, tmpname_p,
				boost::filesystem::copy_option::overwrite_if_exists); // FIXME: error handling?

			// for all syms, do normrelocs <sym> on file tmpname
			for (auto i_sym = claimed_files.back().syms.begin();
				i_sym != claimed_files.back().syms.end();
				++i_sym)
			{
				int ret = normrelocs((char*) tmpname.c_str(), (char*) i_sym->c_str());
				if (ret != 0) abort(); // FIXME: error reporting
			}
			// do ld -r `--defsym othersym` (for all othersyms in claimed_files.back().syms.begin())
			pair<string, int> newtmp = new_temp_file("xwrap-ldplugin-ld-defsymd");
			char *cmdstr;
			int ret = asprintf(&cmdstr, "'%s' -r -o '%s' '%s'", job->ld_cmd.c_str(), newtmp.first.c_str(),
				tmpname.c_str());
			if (ret < 0) abort();
			size_t bufstrlen = ret;
			assert(bufstrlen == strlen(cmdstr));
			size_t buflen = malloc_usable_size(cmdstr);
			for (auto i_sym = claimed_files.back().syms.begin();
				i_sym != claimed_files.back().syms.end();
				++i_sym)
			{
				// realloc the buffer if necessary
				size_t newstrlen = bufstrlen
				+ (sizeof " --defsym __real_" -1) /* -1 for NUL */
				+ i_sym->length()
				+ 1 /* for = */
				+ i_sym->length()
				;
				if (buflen - 1 /* for NUL */ < newstrlen)
				{
					size_t newbuflen = /*MIN*/ ((buflen * 2) > (newstrlen + 1)) ?
						(newstrlen + 1)
						: (buflen * 2);
					cmdstr = reinterpret_cast<char*>(realloc(cmdstr, newbuflen));
					if (!cmdstr) abort();
					buflen = malloc_usable_size(cmdstr);
				}
				assert(bufstrlen <= buflen - 1);
				bufstrlen = strlcat(cmdstr, " --defsym __real_", buflen);
				assert(bufstrlen <= buflen - 1);
				bufstrlen = strlcat(cmdstr, i_sym->c_str(), buflen);
				assert(bufstrlen <= buflen - 1);
				bufstrlen = strlcat(cmdstr, "=", buflen);
				assert(bufstrlen <= buflen - 1);
				bufstrlen = strlcat(cmdstr, i_sym->c_str(), buflen);
				assert(bufstrlen <= buflen - 1);
				assert(bufstrlen == strlen(cmdstr));
			}
			debug_println(0, "system()ing cmdstr: %s", cmdstr);
			ret = system(cmdstr);
			free(cmdstr);
			if (ret != 0) abort(); // FIXME
			// yielding a new temporary file

			// then move that temporary file to tmpname
			boost::filesystem::rename(boost::filesystem::path(newtmp.first), tmpname_p);
		}

		return LDPS_OK;
	}

	/* The plugin library's "all symbols read" handler.  */
	enum ld_plugin_status all_symbols_read()
	{
		debug_println(0, "all-symbols-read handler called ()");
		/* How is this done in, say, the LLVM LTO plugin?
		 * In the claim-file hook, it just claims files and grabs input data.
		 * In the all-symbols-read hook, it creates lots of temporary files
		 *  and does codegen.
		 * How does it feed the generated code back to the linker?
		 * It generates temporary object files and uses add_input_file()
		 * to add them to the link.
		 */

		/* Things we can do in here:
		/* 
			(*add_symbols) (void *handle, int nsyms,
                        			  const struct ld_plugin_symbol *syms);

			(*get_input_file) (const void *handle,
                            			 struct ld_plugin_input_file *file);

			(*get_view) (const void *handle, const void **viewp);

			(*release_input_file) (const void *handle);

			(*get_symbols) (const void *handle, int nsyms,
                        			  struct ld_plugin_symbol *syms);

			(*add_input_file) (const char *pathname);

			(*add_input_library) (const char *libname);
		 */

		for (auto p : claimed_files)
		{
			linker->add_input_file(p.name.c_str());
		}

		/* Also add the extra input files. */
		// if (meta) add_input_file(meta->c_str());

		return LDPS_OK;
	}

	xwrap_plugin(struct ld_plugin_tv *tv) : linker_plugin(tv)
	{
		// no need to call the base 'onload' -- the constructor did the necessary
		/* We need -z muldefs. Restart if we don't have it. */
		RESTART_IF(not_muldefs, missing_option_subseq({"-z", "muldefs"}), job->cmdline);
		debug_println(0, "-z muldefs was%s initially set",
			not_muldefs.did_restart ? " not" : "");

		/* We want to do a pass over the input filenames to generate
		 * firstly a set of objects, and for each identified object,
		 * optionally an 'interesting fact' about it -- here its set
		 * of defined xwrapped symnames. */
		auto input_files = enumerate_input_files(job->cmdline);
		for (auto i_file = input_files.begin(); i_file != input_files.end(); ++i_file)
		{
			debug_println(0, "Input file: %s", i_file->c_str());
		}
		xwrapped_defined_symnames_by_input_file = classify_input_objects< set<string> >(
			input_files,
			[this](fmap const& f, off_t offset, string const& fname) -> set<string> {
				set<pair< ElfW(Sym)*, string > > pairs = enumerate_symbols_matching(f, offset,
					[this](ElfW(Sym)* sym, string const& name) -> bool {
						return (ELFW_ST_TYPE(sym->st_info) == STT_OBJECT
							  ||  ELFW_ST_TYPE(sym->st_info) == STT_FUNC)
							  && (sym->st_shndx != SHN_UNDEF && sym->st_shndx != SHN_ABS)
							  && (std::find(job->options.begin(), job->options.end(), name)
								!= job->options.end());
					});
				// we have a set of pairs... need to return a set of strings
				set<string> ret;
				for (auto i_pair = pairs.begin(); i_pair != pairs.end(); ++i_pair)
				{
					ret.insert(i_pair->second);
				}
				return ret;
			}
		);

		set<string> all_xwrapped_defined_symnames;
		for (auto i_pair = xwrapped_defined_symnames_by_input_file.begin();
			i_pair != xwrapped_defined_symnames_by_input_file.end();
			++i_pair)
		{
			for (auto i_symname = i_pair->second.begin(); i_symname != i_pair->second.end();
				++i_symname)
			{
				debug_println(0, "Xwrapped symname: %s", i_symname->c_str());
				all_xwrapped_defined_symnames.insert(*i_symname);
			}
		}
		/* The --wrap options is tricky. We still need it for the cases
		 * where the wrapped definition is in an external DSO, not in
		 * our link output. Ideally it would look to the user like
		 * 'xwrap' completely subsumes --wrap. */
		auto missing_wrap_options = /* a function that looks for --wrap options and adds any missing */
			[all_xwrapped_defined_symnames, this](vector<string> const& cmdline_vec) -> pair<bool, vector<string> > {
			/* FIXME: can now use linker->get_wrap_symbols() for this. */
			set<string> cmdline_wrapped_syms;
			for (auto i_str = cmdline_vec.begin(); i_str != cmdline_vec.end(); ++i_str)
			{
				if (*i_str == string("--wrap"))
				{
					cmdline_wrapped_syms.insert(cmdline_vec.at(1 + (i_str - cmdline_vec.begin())));
				}
			}
			/* We ensure --wrap options for all our xwrapped symbols
			 * ***that are not defined in an input .o, .a or linker script file***
			 * by adding any missing ones.
			 * We have to iterate over all defined symbols in all input files. */

			set<string> wraps_needed;
			for (auto i_opt = job->options.begin(); i_opt != job->options.end(); ++i_opt)
			{
				if (std::find(all_xwrapped_defined_symnames.begin(),
					all_xwrapped_defined_symnames.end(), *i_opt)
					== all_xwrapped_defined_symnames.end())
				{
					wraps_needed.insert(*i_opt);
				}
			}
			// are there any wraps needed that we don't have?
			set<string> missing;
			for (auto i_needed = wraps_needed.begin(); i_needed != wraps_needed.end(); ++i_needed)
			{
				if (cmdline_wrapped_syms.find(*i_needed) == cmdline_wrapped_syms.end())
				{
					// not found
					missing.insert(*i_needed);
				}
			}
			if (missing.size() > 0)
			{
				vector<string> new_vec = cmdline_vec;
				for (auto i_missing = missing.begin(); i_missing != missing.end(); ++i_missing)
				{
					new_vec.push_back("--wrap");
					new_vec.push_back(*i_missing);
					debug_println(0, "Added missing --wrap option for `%s'", i_missing->c_str());
				}
				return make_pair(true, new_vec);
			}
			return make_pair(false, cmdline_vec);
		};
		RESTART_IF(missing_any_wrap_options, missing_wrap_options, job->cmdline);
		debug_println(0, "all needed wrap options were%s initially set",
			missing_any_wrap_options.did_restart ? " not" : "");

		/* Now we have too much wrap!
		 * We only want it for those files that are not defined locally.
		 * So maybe instead of a restart, this needs to be an error?
		 * i.e. our invoker needs to sort this out.
		 * TRICKY: we want the file to be written before we're invoke,d although maybe
		 * writing it as we go along os OK? Not clear.
		 * I think we can use the check for --wrap to our advantage here.
		 * Assume any --wrap symbol that is also xwrap'd was created
		 * by us.
		 * Then, later, if the set of --wrap symbols is not consistent with
		 * our input (it includes exactly those xwrapped symbols that are not
		 * locally defined, plus maybe some other non-xwrapped symbols)
		 * we error out.
		 * BUT how do we add --wrap when we need it? We don't know exactly
		 * which wraps we need until we've processed the input files. So
		 * we'd be relying on the invoker to supply those, which we didn't want.
		 * I think a compromise on that is sane at this point.
		 *
		 * We could restart with all xwrapped symbols wrapped, and an
		 * extra environment var to tell us that we added those. Then we
		 * tolerate any 'consistent' set of --wrap options but error out
		 * if any is locally defined *and* xwrapped *and* wrapped.
		 * Oh, but then we need a way to remove from the wrap list.
		 *
		 * What is the resource leak issue? It's that if we restart after allocating
		 * a resource that needs cleanup, it won't happen. We're assuming that either
		 * (1) neither ld nor other plugins' "onload" allocate such resources, or.
		 * (2) if other ld plugins do, we run before them.
		 *
		 * Also, what's the issue with just doing the check up-front?
		 * We don't get a series of struct ld_plugin_input_file calls,


		 * FIXME: if the symbol is not defined locally in any input .o file,
		 * but rather in a .so file, bad news: we can't define a real __real_ by fixup.
		 * However, there is also good news: we don't need to, because
		 * it means there are no intra-.o bindings for 'muldefs' to unbind.
		 * Instead we require one half of the original '--wrap' semantics:
		 * resolve __real_foo to foo. (We still capture references to 'foo'
		 * and divert them to '__wrap_foo', so the problem is how the wrapper
		 * refers back to the wrappee, which is in a DSO.)
		 * The fix is to remember which symbols we've handled by fixup,
		 * and emit a --defsym for any that we haven't. OH but that's broken...
		 * 
		 * A possibly unintended consequence is that our wrapper for 'foo'
		 * takes over the global 'foo' identity, which --wrap did not do.
		 * Does this cause problems? E.g. if we're making an executable,
		 * by virtue of *wrapping* calls to 'malloc', it will always
		 * *define* a global 'malloc'. This feels wrong... how do we
		 * ever call into the DSO that really defines the real malloc?
		 * I think we have to change the semantics here. Erk. This is
		 * unpleasant. Basically we can't do caller-side wrapping at present,
		 * because --defsym __real_malloc=malloc will not work.
		 *
		 * I think we want 'xwrap' to use muldefs only if the symbol is to be
		 * defined locally in the output object, and to fall back on plain
		 * --wrap if the symbol is defined in a DSO. i.e. don't defsym and
		 * moreover don't xwrap -- don't add to the linker script. Instead, wrap!
		 * This means we will need another restart to add the possibly-missing
		 * --wrap options. */

	#if 0
		for (auto i_wrapped = wrapped_syms.begin(); i_wrapped != wrapped_syms.end(); ++i_wrapped)
		{
			if (std::find(job->options.begin(), job->options.end(), *i_wrapped) != job->options.end())
			{
				linker->message(LDPL_ERROR, "cannot wrap and xwrap the same symbol (`%s')",
					i_wrapped->c_str());
				return LDPS_ERR;
			}
		}
	#endif

		/* Make a new linker script file, into which we write "sym = __wrap_sym;" lines.
		 * OH, but what if we are re-execing? Then we don't want to create a new
		 * temp file because we will always do this and it will never match our
		 * command line argument. Instead, when we re-exec we will pass the temporary
		 * file name as a /proc/self/fd/%d path and we check for that realpath'ing
		 * to one of our fds. */
		/* Seems we can't do this yet... too early / no workqueue (in gold). */
		// linker->add_input_file(strdup(tmpname.c_str()));
		// So instead, esp as the above probably won't work anyway (need to add
		// at the *beginning* of te link, but the API doesn't let is request this),
		// re-exec with the tmpfile first
		char *tmpldscript_realpathbuf = NULL;
		auto missing_ldscript = /* a function that looks for the ldscript and prepends it if missing */
			[this, &tmpldscript_realpathbuf, all_xwrapped_defined_symnames]
			(vector<string> const& cmdline_vec) -> pair<bool, vector<string> > {
			pair<bool, vector<string> > retval;
			char *path = NULL;
			if (all_xwrapped_defined_symnames.begin() ==
				all_xwrapped_defined_symnames.end())
			{
				// nothing to put in the ldscript, so we're fine
				debug_println(0, "No xwrapped defined symnames!");
				retval = make_pair(false, cmdline_vec);
			}
			else if (STARTS_WITH(string(cmdline_vec.at(1)), "/proc/self/fd/")
				&& STARTS_WITH(string(basename(tmpldscript_realpathbuf = realpath(cmdline_vec[1].c_str(), NULL))),
					"tmp.xwrap-ldplugin-lds"))
			{
				/* We've got it. No need to restart. */
				retval = make_pair(false, cmdline_vec);
			}
			else
			{
				if (tmpldscript_realpathbuf) debug_println(0, "was not impressed by realpath `%s'", tmpldscript_realpathbuf);
				else debug_println(0, "was not impressed with cmdline_vec[1] `%s' (substr: %s)",
					cmdline_vec[1].c_str(),
					cmdline_vec[1].substr(0, sizeof "/proc/self/fd/" - 1).c_str());
				auto tmpfile = new_temp_file("xwrap-ldplugin-lds");
				int tmpfd = tmpfile.second;
				FILE *the_file = fdopen(tmpfd, "w+");
				if (!the_file) abort();
				// FIXME: should only emit for those symbols where we found a definition
				// in an incoming .o or .a file; for the others, do --wrap
				for (auto i_sym = all_xwrapped_defined_symnames.begin();
					i_sym != all_xwrapped_defined_symnames.end(); ++i_sym)
				{
					fprintf(the_file, "%s = __wrap_%s;\n", i_sym->c_str(), i_sym->c_str());
				}
				fflush(the_file);
				std::ostringstream s; s << "/proc/self/fd/" << tmpfd;
				vector<string> new_vec = cmdline_vec;
				new_vec.insert(new_vec.begin() + 1, s.str());
				retval = make_pair(true, new_vec);
			}
			return retval;
		};
		RESTART_IF(no_initial_ldscript, missing_ldscript, job->cmdline);
		debug_println(0, "ldscript was initially %s",
			no_initial_ldscript.did_restart ? "missing yet needed" : "present or unnecessary");
		/* DANGER: we want to avoid leaking the temporary file. We can unlink it, but
		 * only after we're sure we're not going to restart any more, but not before
		 * (else we can't realpath it). So we do this later (see below). */

		if (tmpldscript_realpathbuf)
		{
			// we only refer to the temporary file by its /proc/self/fd name, so...
			// DEBUGGING unlink(tmpldscript_realpathbuf);
			free(tmpldscript_realpathbuf);
		}
		/* That's it for now. We get the all_symbols_read event later.... */
	}
}; // end struct
LINKER_PLUGIN(xwrap_plugin);
