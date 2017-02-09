#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <assert.h>
#include <linux/auxvec.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include "relf.h"

struct dl_open_args /* HACK: pasted from dl-open.c in eglibc 2.19! */
{
  const char *file;
  int mode;
  /* This is the caller of the dlopen() function.  */
  const void *caller_dlopen;
  /* This is the caller of _dl_open().  */
  const void *caller_dl_open;
  struct link_map *map;
  /* Namespace ID.  */
  Lmid_t nsid;
  /* Original parameters to the program and the current environment.  */
  int argc;
  char **argv;
  char **env;
};

/* Here we define dlopen_from_fd.
 * Can we do it using stuff we find in glibc's ld.so?
 * The call we really want is  
 
 struct link_map *
_dl_map_object_from_fd (const char *name, int fd, struct filebuf *fbp,
			char *realname, struct link_map *loader, int l_type,
			int mode, void **stack_endp, Lmid_t nsid)

 * ... which is only non-static if EXTERNAL_MAP_FROM_FD is defined.
 * Otherwise it's called only once, from 

struct link_map *
internal_function
_dl_map_object (struct link_map *loader, const char *name,
		int type, int trace_mode, int mode, Lmid_t nsid)

 * ... though, hmm, mapping an object is only part of the work.
 * We have to load dependencies, do the debugger signalling, etc..
 * Is there not a way we can fake up a file that "looks like" the
 * ELF file we want?
 * 
 * One way is to create a helper process that mmap()s the content
 * at offset zero. Its /proc/$pid/mem is then the ELF file we want.
 * We can write a PIE executable helper that maps <file> <offset>
 * at address 0 in its VAS. Problem is we then have a helper process
 * which has to hang around as long as our meta-object is loaded.
 * OR does it? Once the loader has mmap'd the relevant range of 
 * /proc/$pid/mem, can the process go away? Not really, since it's
 * semantically defined as a view onto that process's memory, not
 * a view of the underlying memory objects in that process's VAS.
 * How can we make those memory objects have names in the fs?
 * One way is to make them POSIX shared memory objects. But we
 * can't just "share" parts of random files... SHM objects have to
 * be created as a blank canvas.
 * 
 * Okay, yes, do the relocation trick... can we avoid the pain of
 * implementing the from_fd function by copying the ELF header
 * somewhere? REMEMBER that we want the dlopen mappings to point
 * *to our executable* just like our own segments do. And that mmap()
 * on /proc/<pid>/mem doesn't work anyway. So no.
 *
 * Okay, how do we turn _dl_map_object_from_fd into a proper
 * dlopen_from_fd? 
 * 
 * Almost all of the logic we want to re-use is in dl_open_worker.
 * This receives all the dlopen's arguments packed into a struct.
 * It calls _dl_map_object *first*, then handles the dependencies,
 * relocations, TLS, etc..
 *
 * What we really want is to take care of mapping the object ourselves,
 * then call this worker to do the other stuff for us.
 * Can we make the _dl_map_object call harmlessly no-op?
 * YES if we can make the soname match.
 *       if (!_dl_name_match_p (name, l))      _dl_name_match_p is in dl-misc.c
 *
 * So how about:
 * 
 * - _dl_map_object_from_fd      which will create the link map entry?
 * - dl_open_worker      with "name" simply set to the soname (nonexistent path)
 *          ... this will match the already-opened entry
 *          ... will it go on to do the other stuff we need?
 *                 ONLY if new->l_searchlist.r_list != NULL
 *              which is only true after mapping the deps, so shouldn't be done yet.
 */

extern char exename[];

struct filebuf
{
  ssize_t len;
#if __WORDSIZE == 32
# define FILEBUF_SIZE 512
#else
# define FILEBUF_SIZE 832
#endif
  char buf[FILEBUF_SIZE] __attribute__ ((aligned (__alignof (ElfW(Ehdr)))));
};

extern char **environ;

extern void **__libc_stack_end;

char *dummy_argv[] = { "dummy", NULL };

void *dlopen_from_fd(int fd, int flag)
{
	static void (*dl_open_worker) (void *a);
	static struct link_map *(*dl_map_object_from_fd) (const char *name, int fd, struct filebuf *fbp,
			char *realname, struct link_map *loader, int l_type,
			int mode, void **stack_endp, Lmid_t nsid);
	int (*dl_catch_error)(const char **objname, const char **errstring,
	             _Bool *mallocedp, void (*operate) (void *), void *args);
	
	static void *ldso_base;
	if (!ldso_base)
	{
		ldso_base = find_ldso_base((const char **) environ, &fd);
		assert(ldso_base);
	}
	if (!dl_open_worker)
	{
		/* HACK */
		dl_open_worker = (void*)((char*) ldso_base + 78208UL);
	}
	if (!dl_map_object_from_fd)
	{
		/* HACK */
		dl_map_object_from_fd = (void*)((char*) ldso_base + 24592UL);
	}
	if (!dl_catch_error)
	{
		/* HACK */
		dl_catch_error = (void*)((char*) ldso_base + 61008UL);
	}
	
	const char *objname;
	const char *errstring;
	_Bool malloced;
	struct filebuf fb;
	off_t off = lseek(fd, 0, SEEK_CUR);
	int ret = read(fd, fb.buf, FILEBUF_SIZE);
	if (ret <= 0)
	{
		return NULL;
	}
	fb.len = ret;
	fprintf(stderr, "read %d bytes from offset 0x%lx\n", (int) fb.len, (long) off);
	
	/* Do the map-object call first. */
	
	struct link_map *ret_lm;
	
	/* Pretend that the target binary is not loaded. This is so that we don't pass 
	 * the inode/dev identity check inside ld.so. FIXME: how to identify the target 
	 * binary in the link map without the private l_ino and l_dev fields? Just use
	 * realpath for now, even though that's broken. */
	char *pathname;
	ret = asprintf(&pathname, "/proc/self/fd/%d", fd);
	if (ret <= 0) return NULL;
	unsigned int saved_l_removed = 0;
	unsigned int *p_l_removed = NULL;
	char realpath_buf[PATH_MAX];
	char *realpath_ret = realpath(pathname, realpath_buf);
	if (realpath_ret) /* i.e. best-effort it if we can't do realpath */
	{
		char *fd_target_filename = strdup(realpath_buf);
		for (struct link_map *l = _r_debug.r_map; l; l = l->l_next)
		{
			const char *objname;
			if (0 == strcmp("", l->l_name))
			{
				/* use the exe name */
				objname = exename;
			} else objname = l->l_name;
			realpath_ret = realpath(objname, realpath_buf);
			if (!realpath_ret)
			{
				/* realpath couldn't work -- keep going */
				continue;
			}
			if (0 == strcmp(realpath_buf, fd_target_filename))
			{
				/* Okay, this is a match, so do our evasion. */
				p_l_removed = (unsigned int *) ((char*) l + /* FIXME: do reflection properly */ 788);
				/* GAH. l_removed is a bitfield. MORE reflection, this time 
				 * using info not exposed in uniqtypes. FIXME. */
				saved_l_removed = *p_l_removed;
				*p_l_removed |= (1u<<13);
			}
		}
		free(fd_target_filename);
	}
	free(pathname);
	
call_site:
	ret_lm = dl_map_object_from_fd(strdup("/blah"), fd, &fb, 
			/* char *realname */ strdup("/blah"), NULL /*struct link_map *loader*/, /* int l_type */ /* lt_library */ 1,
			/* int mode */ RTLD_NOW, __libc_stack_end, /* Lmid_t nsid */ 0);
	if (p_l_removed) *p_l_removed = saved_l_removed;
	/* */
	struct dl_open_args args = {
		/* const char * */ .file = "/blah",
		/* int */ .mode = RTLD_NOW,
		/* This is the caller of the dlopen() function.  */
		/* const void * */ .caller_dlopen = __builtin_return_address(0),
		/* This is the caller of _dl_open().  */
		/* const void * */ .caller_dl_open = &&call_site,
		/* struct link_map * */ .map = NULL,
		/* Namespace ID. */
		/* Lmid_t */ .nsid = 0,
		/* Original parameters to the program and the current environment.  */
		/* int */ .argc = 1,
		/* char ** */ .argv = dummy_argv,
		/* char ** */ .env = environ
	};
	int errcode = dl_catch_error(&objname, &errstring, &malloced,
		dl_open_worker, &args);
	
	if (errcode) return NULL;
	return ret_lm;
	
}
