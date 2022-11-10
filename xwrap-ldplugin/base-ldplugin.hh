#ifndef ELFTIN_BASE_LDPLUGIN_HH_
#define ELFTIN_BASE_LDPLUGIN_HH_

/* To print stuff out, we mostly use the linker MESSAGE interface.
 * But early on, we haven't snarfed it yet. */
#define debug_println(lvl_ignored, fmt, ...) \
    do { \
       if (::linker && ::linker->message) ::linker->message(LDPL_INFO, fmt __VA_OPT__(,) ## __VA_ARGS__ ); \
       else { fprintf(stderr, fmt "\n" __VA_OPT__(,) ## __VA_ARGS__); fflush(stderr); } \
        /* HMM. fflushing stderr no longer works! Delay but no message until too late. */ \
        /* Is this an artifact of prettified output? */ \
        /* stderr is going to a file in /tmp /tmp/ccQhnr0p.le */ \
        /* -fno-diagnostics-color has no effect. */ \
        /* Redirecting output  2>&1 |cat has no effect. */ \
        /* Using gold makes no difference. */ \
        /* HMM. GCC 10 is fine. GCC 8.3 causes the problem. */ \
    } while (0)

#include <string>
#include <vector>
#include <utility>
#include <memory>
#include "plugin-api.hh"
#include <srk31/closure.hpp> /* for pointer-to-member closures using libffi */

/* Does C++ifying the plugin interface make sense?
 * We could get away with just C-style library code for the shared stuff.
 *
 * One apparent win of C++ifying is that we can do overriding of virtual
 * functions. If we didn't have that, we'd end up using __wrap_
 * or would have a naming convention for 'extended_blah_handler' that calls
 * down to 'blah_handler', etc.
 *
 * More significantly, we would then have to break down the 'register_()' calls
 * so that our plugin registered its own implementation code, maybe by taking
 * multiple passes over the transfer vector and attending to different subsets
 * each time -- registering the handlers, but leaving library code to slurp the
 * linker utility functions, say. That seems fragile, e.g. we could clobber
 * a handler by 'registering' it twice.
 * With the C++ified version, the transfer vector logic is centralised
 * in the constructor -- but it can still register a derived implementation,
 * thanks to virtuality. (We'd have to use --wrap to get that effect.)
 * We also have some structure to how we manage the base versus derived state:
 * we initialize it in constructors (base first),
 * and we have scoping rules for how we refer to it (can see base from derived
 * but not vice-versa).
 * On the other hand, we have code that is 'more dynamic than necessary',
 * passing around pointers that could be optimised out, doing vtable dispatches
 * that could be resolved ahead of time, etc.
 * Since we're writing the plugins in C++ anyway, I'll take the C++ified version. */

namespace elftin {

using std::string;
using std::vector;
using std::unique_ptr;
using std::pair;

/* For any datum the transfer vector might give us,
 * put a member variable here. */
struct link_job
{
	int output_file_type;
	string output_file_name;
	vector<string> options; // the plugin options
	string ld_cmd;          // the ld's argv[0]
	vector<string> cmdline; // the whole ld command line
	link_job() : output_file_type(-1) {}
};
/* The transfer vector also gives us a bunch of functions we can
 * call on the linker itself. These will be stored in an instance
 * of this structure, defined in plugin-api.hh. */
struct linker_s;

/* For any 'hook' the transfer vector gives us a 'register' call for,
 * put a member function here. These are the operations the linker
 * will call on our plugin. */
struct linker_plugin
{
	unique_ptr<link_job> job;
	unique_ptr<linker_s> linker;
	/* Conceptually we have a bunch of member functions, taking a 'this'.
	 * We will also dynamically generate a 'this'-less closure, which is actually
	 * what gets registered with the linker. So preprocess this up. */
#define member_functions(v, w) \
    v(enum ld_plugin_status, claim_file, w(const struct ld_plugin_input_file *, file), w(int *, claimed)) \
    v(enum ld_plugin_status, all_symbols_read) \
    v(enum ld_plugin_status, new_input, w(const struct ld_plugin_input_file *, file))
#define type_and_name(t, n) t n
#define type_only(t, n) t
#define declare_member(rett, name, args...) virtual rett name( args );
	member_functions(declare_member, type_and_name)
#define declare_member_closure(rett, name, ...) \
    unique_ptr<rett( __VA_ARGS__ ), srk31::ffi_closure_s<linker_plugin, rett __VA_OPT__(,) __VA_ARGS__>::closure_deleter > name ## _closure;
	member_functions(declare_member_closure, type_only)
	int do_not_use; /* comma termination hack -- constructor initializes this member last */
	/* processes the transfer vector and wires us up, creating 'job' and 'linker' */
	linker_plugin(struct ld_plugin_tv *tv);
	virtual ~linker_plugin() {}
	/* calling cleanup is a bug */
	enum ld_plugin_status cleanup()
	{ debug_println(0, "BUG: cleanup called; use top-level cleanup, calling destructor"); return LDPS_ERR; }
	/* 'onload' is special: it must be defined as the unique C-linkage symbol
	 * named 'onload' in the plugin .so. Provide a macro to generate this code
	 * for whatever plugin class we want to instantiate. No point making onload
	 * virtual, since it's always called directly, just like a constructor.
	 * Similarly, 'cleanup' is not a method; we hard-code a cleanup handler
	 * that forces destruction of the singleton plugin object. */
#define LINKER_PLUGIN(plugin_t) \
	extern "C" { \
	  enum ld_plugin_status \
	  onload(struct ld_plugin_tv *tv); \
	} \
	static std::unique_ptr<linker_plugin> plugin; \
	struct linker_s *linker; \
	enum ld_plugin_status \
	cleanup(void) \
	{ \
		linker = nullptr; \
		plugin.reset(); \
		return LDPS_OK; \
	}; \
	enum ld_plugin_status \
	onload(struct ld_plugin_tv *tv) \
	{ \
		plugin = std::make_unique<plugin_t>(tv); \
		linker = plugin->linker.get(); \
		linker->register_cleanup(cleanup); \
		return LDPS_OK; \
	};

protected:
	/* utility code*/
	/* FIXME: actually unlink.
	 * FIXME: make this robust across restarts.
	 * FIXME: do something about other plugins / core ld leaking resources across
	 * restarts (warn about fds >= 3 to linked files or non-CLOEXEC?) */
	vector<string> temp_files_to_unlink;
	pair<string, int> new_temp_file(const string& insert);
};

} /* end namespace elftin */

#endif
