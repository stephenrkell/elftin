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
#include <boost/optional.hpp>
extern "C" {
#include <link.h>
}
#include "relf.h" /* from librunt; used to get our ld binary's realpath -- bit of a HACK */
#include "elfmap.hh"

/* To print stuff out, we mostly use the linker MESSAGE interface. 
 * But early on, we haven't snarfed it yet. */
#define debug_println(lvl_ignored, fmt, ...) \
    do { \
       if (linker->message) linker->message(LDPL_INFO, fmt, ## __VA_ARGS__ ); \
       else { fprintf(stderr, fmt "\n", ## __VA_ARGS__); fflush(stderr); } \
        /* HMM. fflushing stderr no longer works! Delay but no message until too late. */ \
        /* Is this an artifact of prettified output? */ \
        /* stderr is going to a file in /tmp /tmp/ccQhnr0p.le */ \
        /* -fno-diagnostics-color has no effect. */ \
        /* Redirecting output  2>&1 |cat has no effect. */ \
        /* Using gold makes no difference. */ \
        /* HMM. GCC 10 is fine. GCC 8.3 causes the problem. */ \
    } while (0)

#include "plugin-api.hh"
#include "restart-self.hh"

using std::vector;
using std::string;
using std::smatch;
using std::regex;
using std::regex_match;
using std::pair;
using std::make_pair;
using boost::optional;

using namespace elftin;

struct linker linker[1];
std::vector<string> options;
string output_file_name;

vector<pair<string, const Elf64_Sym *> >
symbols_with(const Elf64_Sym *symtab, 
	const Elf64_Sym *symtab_end,
	const char *strtab,
	const std::function<bool(const Elf64_Sym *, const string&)>& f
)
{
	vector<pair<string, const Elf64_Sym *> > out;
	for (; symtab != symtab_end; ++symtab)
	{
		string name = strtab + symtab->st_name;
		if (f(symtab, name)) out.push_back(make_pair(name, symtab));
	}
	return out;
}

vector<pair<string, const Elf64_Sym *> >
symbols_matching(const Elf64_Sym *symtab,
	const Elf64_Sym *symtab_end,
	const char *strtab,
	const vector<string>& patterns /* must be alternation-free */,
	const std::function<bool(const Elf64_Sym *, const string&)>& extra_pred) // must be alternation-free
{
	string big_pattern;
	std::ostringstream s;
	for (auto i = patterns.begin(); i != patterns.end(); ++i)
	{
		if (i != patterns.begin()) s << "|";
		s << *i;
	}
	big_pattern = s.str();
	// debug_println(0, "Looking for symbols matching big pattern: %s", big_pattern.c_str());
	auto pred = [big_pattern, extra_pred](const Elf64_Sym *s, const string& name) {
		if (extra_pred(s, name))
		{
			smatch m;
			if (regex_match(name, m, regex(big_pattern)))
			{
				debug_println(0, "Saw symbol matching pred and pattern (%s): %s", 
					big_pattern.c_str(), name.c_str());
				return true;
			}
		}
		return false;
	};
	return symbols_with(symtab, symtab_end, strtab, pred);
}

vector<pair<string, const Elf64_Sym *> >
defined_symbols_matching(const Elf64_Sym *symtab, 
	const Elf64_Sym *symtab_end,
	const char *strtab,
	const vector<string>& patterns) // must be alternation-free
{
	return symbols_matching(symtab, symtab_end, strtab, patterns, 
		[](const Elf64_Sym *s, const string& f) {
			return s->st_shndx != SHN_UNDEF;
		});
}

vector<pair<string, const Elf64_Sym *> >
undefined_symbols_matching(const Elf64_Sym *symtab, 
	const Elf64_Sym *symtab_end,
	const char *strtab,
	const vector<string>& patterns) // must be alternation-free
{
	return symbols_matching(symtab, symtab_end, strtab, patterns, 
		[](const Elf64_Sym *s, const string& f) {
			return s->st_shndx == SHN_UNDEF;
		});
}

static vector<string> temp_files_to_unlink;
pair<string, int> new_temp_file(const string& insert)
{
	// FIXME: don't hard-code "/tmp"
	char *tempnambuf = strdup(("/tmp/tmp." + insert + ".XXXXXX").c_str());
	int tmpfd = mkstemp(tempnambuf);
	if (tmpfd == -1) abort(); // FIXME: better diagnostics
	string tempnam = tempnambuf;
	free(tempnambuf);
	temp_files_to_unlink.push_back(tempnam);
	return make_pair(tempnam, tmpfd);
}

/* These will be initialized from the transfer vector. */
static int output_file_type = -1;
static int api_version;
// only one of these two will be set
static int gold_version;
static int gnu_ld_version;

string path_to_fixup_script;

/* The plugin library's "claim file" handler.  */
struct claimed_file
{
	const struct ld_plugin_input_file *input_file;
	string name;
	vector<string> syms;

	claimed_file(const std::pair<const struct ld_plugin_input_file *, string> & p)
		: input_file(p.first), name(p.second) {}
};
static vector< claimed_file > claimed_files;
static vector< const struct ld_plugin_input_file * > input_files;
static
enum ld_plugin_status
claim_file_handler (
  const struct ld_plugin_input_file *file, int *claimed)
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
	// auto ret = get_symbols(file, 0, nullptr);
	bool we_claim_it = false;
	elfmap e(file->fd, file->offset);
	if (e && 0 == strncmp((const char *) e.hdr->e_ident, "\x7f""ELF", 4))
	{
		debug_println(0, "Mapped an ELF at %p", e.mapping);
		if (e.hdr->e_ident[EI_CLASS] == ELFCLASS64 &&
			e.hdr->e_ident[EI_DATA] == ELFDATA2LSB &&
			e.hdr->e_type == ET_REL) // FIXME: archives
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
				/* Walk the symtab looking for names that match */
				for (ElfW(Sym) *sym = symtab; (uintptr_t) sym < (uintptr_t) symtab + found->sh_size; ++sym)
				{
					// claim the file if it defines a wrapped symbol
					const char *name = &strtab[sym->st_name];
					if (( ELFW_ST_TYPE(sym->st_info) == STT_OBJECT
					  ||  ELFW_ST_TYPE(sym->st_info) == STT_FUNC)
					  && (sym->st_shndx != SHN_UNDEF && sym->st_shndx != SHN_ABS)
					  && std::find(options.begin(), options.end(), string(name))
						!= options.end())
					{
						if (!*claimed)
						{
							*claimed = 1;
							/* Make a temp that will stand in for this file. */
							auto tmpfile = new_temp_file("xwrap-ldplugin");
							string tmpname = tmpfile.first;
							int tmpfd = tmpfile.second;
							if (tmpfd == -1) abort();
							debug_println(0, "Claimed file is replaced by temporary %s", tmpname.c_str());
							claimed_files.push_back(make_pair(file, tmpname));
						}
						claimed_files.back().syms.push_back(name);
					}
				}
			}
		} /* end if is an interesting ELF file */
		if (*claimed)
		{
			/* run the script to generate the file */
			string cmd = path_to_fixup_script + " "
				+ claimed_files.back().name + " " +  claimed_files.back().input_file->name;
			for (auto i_sym = claimed_files.back().syms.begin();
				i_sym != claimed_files.back().syms.end();
				++i_sym)
			{
				cmd += string(" ") + *i_sym;
			}
			int ret = system(cmd.c_str());
			// now claim the fixed-up file
		}
	}

	// do we really want to do this?
	input_files.push_back(file);

	return LDPS_OK;
}

/* The plugin library's "all symbols read" handler.  */
static
enum ld_plugin_status
all_symbols_read_handler (void)
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

static enum ld_plugin_status
new_input_handler(const struct ld_plugin_input_file *file)
{
	debug_println(0, "new input handler called ()");
	return LDPS_OK;
}

static
enum ld_plugin_status
cleanup_handler (void)
{
	debug_println(0, "cleanup handler called ()");
	return LDPS_OK;
}

/* The plugin library's "onload" entry point.  */
extern "C" {
enum ld_plugin_status
onload(struct ld_plugin_tv *tv);
}
enum ld_plugin_status
onload(struct ld_plugin_tv *tv)
{
	// for debugging
	if (getenv("LD_DELAY_STARTUP"))
	{
		debug_println(0, "Hello from linker plugin, in pid %d", getpid());
		sleep(12);
		unsetenv("LD_DELAY_STARTUP"); // so that restarts don't delay
	}
#define CASE(x) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x ", arg %p", i_tv->tv_u.tv_string);
#define CASE_INT(x, dest) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x ", arg %d", i_tv->tv_u.tv_val); dest = i_tv->tv_u.tv_val;
#define CASE_STRING(x, dest) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x ", arg `%s'", i_tv->tv_u.tv_string); dest = i_tv->tv_u.tv_string;
#define CASE_STRINGMANY(x, destvec) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x ", arg `%s'", i_tv->tv_u.tv_string); destvec.push_back(i_tv->tv_u.tv_string);
#define CASE_FP(x, lc) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x "; argument %p", \
	    i_tv->tv_u.tv_ ## lc); \
	linker->lc = static_cast<__typeof(linker->lc)>(i_tv->tv_u.tv_ ## lc);
#define CASE_FP_REGISTER(x, lc) \
	case LDPT_REGISTER_ ## x: debug_println(0, "Transfer vector contained LDPT_REGISTER_" #x "; argument %p", \
	    i_tv->tv_u.tv_register_ ## lc); \
	linker->register_ ## lc = static_cast<__typeof(linker->register_ ## lc)>(i_tv->tv_u.tv_register_ ## lc); \
	linker->register_ ## lc ( lc ## _handler );
	for (struct ld_plugin_tv *i_tv = tv; i_tv->tv_tag != LDPT_NULL; ++i_tv)
	{
		switch (i_tv->tv_tag)
		{
			CASE(NULL) break;
			CASE_INT(API_VERSION, api_version) break;
			CASE_INT(GOLD_VERSION, gold_version) break;
			CASE_INT(LINKER_OUTPUT, output_file_type) break;
			CASE_STRINGMANY(OPTION, options) /* This is -plugin-opt */ break;
			CASE_FP_REGISTER(CLAIM_FILE_HOOK, claim_file) break;
			CASE_FP_REGISTER(ALL_SYMBOLS_READ_HOOK, all_symbols_read) break;
			CASE_FP_REGISTER(CLEANUP_HOOK, cleanup) break;
			CASE_FP(ADD_SYMBOLS, add_symbols) break;
			CASE_FP(GET_SYMBOLS, get_symbols) break;
			CASE_FP(ADD_INPUT_FILE, add_input_file) break;
			CASE_FP(MESSAGE, message) break;
			CASE_FP(GET_INPUT_FILE, get_input_file) break;
			CASE_FP(RELEASE_INPUT_FILE, release_input_file) break;
			CASE_FP(ADD_INPUT_LIBRARY, add_input_library) break;
			CASE_STRING(OUTPUT_NAME, output_file_name) break;
			CASE_FP(SET_EXTRA_LIBRARY_PATH, set_extra_library_path) break;
			CASE_INT(GNU_LD_VERSION, gnu_ld_version) break;
			CASE_FP(GET_VIEW, get_view) break;
			CASE_FP(GET_INPUT_SECTION_COUNT, get_input_section_count) break;
			CASE_FP(GET_INPUT_SECTION_TYPE, get_input_section_type) break;
			CASE_FP(GET_INPUT_SECTION_NAME, get_input_section_name) break;
			CASE_FP(GET_INPUT_SECTION_CONTENTS, get_input_section_contents) break;
			CASE_FP(UPDATE_SECTION_ORDER, update_section_order) break;
			CASE_FP(ALLOW_SECTION_ORDERING, allow_section_ordering) break;
			CASE_FP(GET_SYMBOLS_V2, get_symbols) break;
			CASE_FP(ALLOW_UNIQUE_SEGMENT_FOR_SECTIONS, allow_unique_segment_for_sections) break;
			CASE_FP(UNIQUE_SEGMENT_FOR_SECTIONS, unique_segment_for_sections) break;
			CASE(GET_SYMBOLS_V3) break;
			CASE_FP(GET_INPUT_SECTION_ALIGNMENT, get_input_section_alignment) break;
			CASE_FP(GET_INPUT_SECTION_SIZE, get_input_section_size) break;
			CASE_FP_REGISTER(NEW_INPUT_HOOK, new_input) break;
			CASE_FP(GET_WRAP_SYMBOLS, get_wrap_symbols) break;
			default:
				debug_println(0, "Did not recognise transfer vector element %d", 
					(int) i_tv->tv_tag);
				break;
		}
	}

	/* Let's also inspect our command line. If we don't like it,
	 * we may want to re-exec ourselves. */
	ElfW(auxv_t) *auxv = get_auxv_via_environ(environ, &auxv, (void*)-1);
	assert(auxv);
	struct auxv_limits auxv_limits = get_auxv_limits(auxv);
	for (const char **p = auxv_limits.argv_vector_start; *p; ++p)
	{
		debug_println(0, "Saw arg: `%s'", *p);
	}
	/* Sanity check: if we're wrapping, let's not also xwrap.
	 * FIXME: can now use linker->get_wrap_symbols() for this. */
	vector<string> wrapped_syms;
	for (const char **p = auxv_limits.argv_vector_start; *p; ++p)
	{
		if (*p == string("--wrap"))
		{
			wrapped_syms.push_back(*(p+1));
		}
	}
	for (auto i_wrapped = wrapped_syms.begin(); i_wrapped != wrapped_syms.end(); ++i_wrapped)
	{
		if (std::find(options.begin(), options.end(), *i_wrapped) != options.end())
		{
			linker->message(LDPL_ERROR, "cannot wrap and xwrap the same symbol (`%s')", i_wrapped->c_str());
			return LDPS_ERR;
		}
	}

	/* We need -z muldefs. Restart if we don't have it. */
	RESTART_IF(not_muldefs, missing_option_subseq({"-z", "muldefs"}));
	debug_println(0, "-z muldefs was%s previously set",
		not_muldefs.did_restart ? " not" : "");

	/* Make a new linker script file, into which we write "sym = __wrap_sym;" lines.
	 * OH, but what if we are re-execing? Then we don't want to create a new
	 * temp file because we will always do this and it will never match our
	 * command line argument. Instead, when we re-exec we will pass the temporary
	 * file name as a /proc/self/fd/%d path and we check for that realpath'ing
	 * to one of our fds. */
	/* Seems we can't do this yet... too early / no workqueue (in gold). */
	// linker->add_input_file(strdup(tmpname.c_str()));
	// So instead, esp as the above probably won't work anyway, re-exec with the tmpfile first
	char *tmpldscript_realpathbuf = NULL;
	auto missing_ldscript = /* a function that looks for the ldscript and prepends it if missing */
		[&tmpldscript_realpathbuf](const char **cmdline) -> pair<bool, vector<string> > {
		vector<string> cmdline_vec;
		pair<bool, vector<string> > retval;
		for (const char **p = cmdline; *p; ++p) cmdline_vec.push_back(*p);
		char *path = NULL;
#define STARTS_WITH(var, conststr) \
(((var).substr(0, sizeof conststr - 1) == string(conststr)))
		if (STARTS_WITH(string(cmdline[1]), "/proc/self/fd/")
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
			for (auto i_sym = options.begin(); i_sym != options.end(); ++i_sym)
			{
				fprintf(the_file, "%s = __wrap_%s;\n", i_sym->c_str(), i_sym->c_str());
			}
			fflush(the_file);
			std::ostringstream s; s << "/proc/self/fd/" << tmpfd;
			cmdline_vec.insert(cmdline_vec.begin() + 1, s.str());
			retval = make_pair(true, cmdline_vec);
		}
		return retval;
	};
	RESTART_IF(no_initial_ldscript, missing_ldscript);
	debug_println(0, "ldscript was previously %s",
		no_initial_ldscript.did_restart ? "missing" : "present");
	/* DANGER: we want to avoid leaking the temporary file. We can unlink it, but
	 * only after we're sure we're not going to restart any more, but not before
	 * (else we can't realpath it). So we do this later (see below). */

	/* Snarf the path to our fixup script. */
	path_to_fixup_script = string(dirname(get_link_map((void*) onload)->l_name)) + "/xwrap-fixup.sh";



	if (tmpldscript_realpathbuf)
	{
		// we only refer to the temporary file by its /proc/self/fd name, so...
		// DEBUGGING unlink(tmpldscript_realpathbuf);
		free(tmpldscript_realpathbuf);
	}
	/* That's it for now. We get the all_symbols_read event later.... */
	return LDPS_OK;
}