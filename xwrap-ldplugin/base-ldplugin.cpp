#include <vector>
#include <set>
#include <map>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <libgen.h> /* for dirname() and GNU basename() -- must include before cstring */
#include <cassert>
#include <unistd.h> /* for sleep() */
#include <utility> /* for pair */
#include <boost/optional.hpp>
extern "C" {
#include <link.h>
}

#include "base-ldplugin.hh"
#include "relf.h" /* from librunt; used to get our ld binary's realpath -- bit of a HACK */

using std::vector;
using std::string;
using std::set;
using std::map;
using std::pair;
using std::make_pair;
using boost::optional;

namespace elftin {

#define initialize_closure_to_nullptr(rett, name, ...) \
  name ##_closure (nullptr, srk31::ffi_closure_s<linker_plugin, rett __VA_OPT__(,) __VA_ARGS__>::closure_deleter()),
linker_plugin::linker_plugin(struct ld_plugin_tv *tv)
: member_functions(initialize_closure_to_nullptr, type_only)
  do_not_use(0) /* comma termination hack */
{
	this->linker = std::make_unique<linker_s>();
	this->job = std::make_unique<link_job>();
	// for debugging
	if (getenv("LD_DELAY_STARTUP"))
	{
		debug_println(0, "Hello from linker plugin, in pid %d", getpid());
		sleep(12);
		unsetenv("LD_DELAY_STARTUP"); // so that restarts don't delay
	}
/* Any macro with 'dest' or 'destvec' means we snarf the info into 'job'.
 * With 'register' we set the plugin object's member functions as handlers. */
#define CASE(x) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x ", arg %p", i_tv->tv_u.tv_string);
#define CASE_INT(x, dest) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x ", arg %d", i_tv->tv_u.tv_val); job->dest = i_tv->tv_u.tv_val;
#define CASE_STRING(x, dest) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x ", arg `%s'", i_tv->tv_u.tv_string); job->dest = i_tv->tv_u.tv_string;
#define CASE_STRINGMANY(x, destvec) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x ", arg `%s'", i_tv->tv_u.tv_string); job->destvec.push_back(i_tv->tv_u.tv_string);
#define CASE_FP(x, lc) \
	case LDPT_ ## x: debug_println(0, "Transfer vector contained LDPT_" #x "; argument %p", \
	    i_tv->tv_u.tv_ ## lc); \
	linker->lc = static_cast<__typeof(linker->lc)>(i_tv->tv_u.tv_ ## lc);
#define CASE_FP_REGISTER(x, lc) \
	case LDPT_REGISTER_ ## x: debug_println(0, "Transfer vector contained LDPT_REGISTER_" #x "; argument %p", \
	    i_tv->tv_u.tv_register_ ## lc); \
	/* snarf the register_* function, in *linker */ \
	linker->register_ ## lc = static_cast<__typeof(linker->register_ ## lc)>(i_tv->tv_u.tv_register_ ## lc); \
	/* make the closure */ \
	/* We name 'linker_plugin' (us) here, but the generated code does      */ \
	/*                            (p->*m)(...)                             */ \
	/* and the compiler knows whether *p has polymorphic type. So it will  */ \
	/* generate the virtual dispatch code at this point. Though the member */ \
	/* function needs to be virtual -- hiding alone won't do it.           */ \
	this->lc ## _closure = std::move( \
	    srk31::member_fun_typedef< __typeof( &linker_plugin:: lc ) >::t:: \
	        make_closure< &linker_plugin::lc >(this) \
	    ); \
	/* *call* the register_* function, passing our member function (closure'd) */ \
	linker->register_ ## lc ( this->lc ## _closure.get() );
	for (struct ld_plugin_tv *i_tv = tv; i_tv->tv_tag != LDPT_NULL; ++i_tv)
	{
		switch (i_tv->tv_tag)
		{
			CASE(NULL) break;
			CASE(API_VERSION) break; /* FIXME: check we grok this */
			CASE(GOLD_VERSION) break; /* FIXME: check we grok this */
			CASE_INT(LINKER_OUTPUT, output_file_type) break;
			CASE_STRINGMANY(OPTION, options) /* This is -plugin-opt */ break;
			CASE_FP_REGISTER(CLAIM_FILE_HOOK, claim_file) break;
			CASE_FP_REGISTER(ALL_SYMBOLS_READ_HOOK, all_symbols_read) break;
			CASE_FP(REGISTER_CLEANUP_HOOK, register_cleanup) break; // NOT registered -- use destructor
			CASE_FP(ADD_SYMBOLS, add_symbols) break;
			CASE_FP(GET_SYMBOLS, get_symbols) break;
			CASE_FP(ADD_INPUT_FILE, add_input_file) break;
			CASE_FP(MESSAGE, message) break;
			CASE_FP(GET_INPUT_FILE, get_input_file) break;
			CASE_FP(RELEASE_INPUT_FILE, release_input_file) break;
			CASE_FP(ADD_INPUT_LIBRARY, add_input_library) break;
			CASE_STRING(OUTPUT_NAME, output_file_name) break;
			CASE_FP(SET_EXTRA_LIBRARY_PATH, set_extra_library_path) break;
			CASE(GNU_LD_VERSION) break; /* FIXME: check we grok this */
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
			//CASE_FP(GET_WRAP_SYMBOLS, get_wrap_symbols) break;
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
		job->cmdline.push_back(*p);
	}
	job->ld_cmd = job->cmdline.at(0);
}

/* utility code */

pair<string, int> linker_plugin::new_temp_file(const string& insert)
{
	char *tempnambuf = strdup(
		(string(getenv("TMPDIR")?:"/tmp") + "/tmp." + insert + ".XXXXXX").c_str()
	);
	int tmpfd = mkstemp(tempnambuf);
	if (tmpfd == -1) abort(); // FIXME: better diagnostics
	string tempnam = tempnambuf;
	free(tempnambuf);
	temp_files_to_unlink.push_back(tempnam);
	return make_pair(tempnam, tmpfd);
}

/* default implementations */

enum ld_plugin_status
linker_plugin::claim_file (
  const struct ld_plugin_input_file *file, int *claimed)
{
	debug_println(0, "claim-file handler called (on `%s', currently %d)", file->name, *claimed);
	return LDPS_OK;
}

/* The plugin library's "all symbols read" handler.  */
enum ld_plugin_status
linker_plugin::all_symbols_read()
{
	debug_println(0, "all-symbols-read handler called ()");
	return LDPS_OK;
}

enum ld_plugin_status
linker_plugin::new_input(const struct ld_plugin_input_file *file)
{
	debug_println(0, "new input handler called ()");
	return LDPS_OK;
}

} /* end namespace elftin */
