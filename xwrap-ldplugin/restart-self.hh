#ifndef RESTART_SELF_HH_
#define RESTART_SELF_HH_

#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <utility>
#include <err.h>

#define getenv_ignoring_equals(key_with_equals) ({ \
     size_t bufsz = strlen(key_with_equals); \
     char buf[bufsz]; \
     memcpy(buf, key_with_equals, bufsz); \
     buf[bufsz - 1] = '\0'; \
     getenv(buf); \
})

#include "plugin-api.hh"
#include "base-ldplugin.hh"

namespace elftin
{

using std::vector;
using std::string;
using std::pair;
using std::make_pair;

/* Want a restart mechanism that is really clean. Something like this.

   restart_if not_muldefs(missing_option_subseq({"-z", "muldefs"}));

 ... with the semantics that restart will ensure the arg pred is true on restart
 ... and later be able to query

 not_muldefs.true_before_restart()

 * Is the right thing to do just to pass the old command line, in toto,
 * as an env var? Feels cleaner in a way, but we would have to quote it
 * and also parse it. Another thing is somehow to code up which parts
 * of the reexec'd command line we've messed with. That is also nasty.
 */

/* A restart criterion is a function over command lines that
 * - firstly tells you whether we need to restart
 * - secondly gives you a fixed-up command line, with which to restart, such that
 *     there will be *no* need to restart next time. */
typedef std::function<pair<bool,vector<string> >(vector<string> const&)> restart_criterion;

/* missing_option_subseq is a restart criterion that tests whether
 * the command line contains a given contiguous subsequence,
 * and if it doesn't, creates a new command line that does. */
auto missing_option_subseq = [](const vector<string>& seq) -> restart_criterion {
	/* return a function that looks for the subseq and appends if it missing */
	auto the_f = [seq](vector<string> const& cmdline_vec) -> pair<bool, vector<string> > {
		auto match_found = [cmdline_vec, seq]() -> bool {
			for (auto i = cmdline_vec.begin(); i != cmdline_vec.end(); ++i)
			{
				// try to find a match starting here
				auto j = i;
				bool matched = true;
				while (j != i + seq.size())
				{
					unsigned offset = j-i;
					if (j == cmdline_vec.end() /* ran out of cmdline */
						|| *j != seq[offset]) { matched = false; break; }
					++j;
				}
				if (matched) return true;
			}
			return false;
		};
		if (!match_found())
		{
			vector<string> new_vec = cmdline_vec;
			// append the subseq and return 'yes, restart with this'
			for (auto i_seq = seq.begin(); i_seq != seq.end(); ++i_seq)
			{
				new_vec.push_back(*i_seq);
			}
			return make_pair(true, new_vec);
		}
		return make_pair(false, vector<string>());
	};
	return the_f;
};

/* restart_if represents a condition on which we need to restart
 * with a modified command line (that will falsify the condition).
 *
 * When it restarts, it puts a guard in the environment.
 * So on creation, if the guard exists and the condition is true,
 * it's an error because the fixup logic didn't prevent the condition.
 * Else if the guard exists, it means we restarted and the condition
 * was previously true (but now isn't); we remember this.
 * Else if the condition is true, we need to add the guard and restart
 * using the fixed-up command line.
 * Else if the condition is false, we remember this and continue.
 */
struct restart_if
{
	string mangle(const string& s) const
	{
		string mangled = s;
		std::replace_if(mangled.begin(), mangled.end(), [](char c) -> bool {
			return c != '_' && (c < '0' || c > 'z');
		}, '_');
		return "LD_PLUGIN_RESTART_GUARD_" + mangled;
	}
	int do_restart(const vector<string>& cmdline)
	{
		unsigned long nchars = 0;
		for (auto i = cmdline.begin(); i != cmdline.end(); ++i)
		{
			nchars += i->length() + 1;
		}
		char *argv[cmdline.size() + 1];
		char **argvpos = &argv[0];
		char buf[nchars];
		debug_println(0, "buf at %p is %d chars\n", buf, (int) nchars);
		char *bufpos = &buf[0];
		for (auto i = cmdline.begin(); i != cmdline.end(); ++i)
		{
			*argvpos++ = bufpos;
			unsigned len = strlen(i->c_str());
			memcpy(bufpos, i->c_str(), len+1);
			bufpos += len+1;
		}
		argv[cmdline.size()] = NULL;
		fflush(stdout);
		fflush(stderr);
		char *exepath = realpath("/proc/self/exe", NULL);
		argv[0] = exepath;
		return execve(exepath, argv, environ); // should not return!
	}
	bool did_restart;
	restart_if(restart_criterion cond,
		       const char *condstr,
		       vector<string> const& cmdline_vec)
	{
		string guard = mangle(condstr);
		auto retpair = cond(cmdline_vec);
		if (retpair.first && getenv(guard.c_str()))
		{
			// this is a pure logic error... should not happen
			abort();
		}
		else if (retpair.first)
		{
			putenv(strdup((guard + "=").c_str()));
			do_restart(retpair.second);
			// exec failed... why?
			err(EXIT_FAILURE, "self-execing for reason `%s'", condstr);
		}
		else if (getenv(guard.c_str()))
		{
			did_restart = true;
		}
		else 
		{
			/* the good case */
			did_restart = false;
		}
	}
};
#define stringifya(...) # __VA_ARGS__
// stringify expanded
#define stringifxa(...) stringifya(__VA_ARGS__)
#define RESTART_IF(id, cond, cmdvec) \
	restart_if id(cond, stringifxa(cond), cmdvec)

} /* end namespace elftin */

#endif
