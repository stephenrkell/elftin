#ifndef PLUGIN_API_HH_
#define PLUGIN_API_HH_

/* Linker interfaces: direct interaction.
 * These are populated from the transfer vector.
 * PROBLEM: even for the same LDPT_API_VERSION,
 * we can get different subsets of the interface.
 * So we have to check for the presence of the operations
 * we care about. */
struct linker {

/* The linker's interface for adding symbols from a claimed input file.  */
enum ld_plugin_status
(*add_symbols) (void *handle, int nsyms,
                          const struct ld_plugin_symbol *syms);

/* The linker's interface for getting the input file information with
   an open (possibly re-opened) file descriptor.  */
enum ld_plugin_status
(*get_input_file) (const void *handle,
                             struct ld_plugin_input_file *file);

enum ld_plugin_status
(*get_view) (const void *handle, const void **viewp);

/* The linker's interface for releasing the input file.  */
enum ld_plugin_status
(*release_input_file) (const void *handle);

/* The linker's interface for retrieving symbol resolution information.  */
enum ld_plugin_status
(*get_symbols) (const void *handle, int nsyms,
                          struct ld_plugin_symbol *syms);

/* The linker's interface for adding a compiled input file.  */
enum ld_plugin_status
(*add_input_file) (const char *pathname);

/* The linker's interface for adding a library that should be searched.  */
enum ld_plugin_status
(*add_input_library) (const char *libname);

/* The linker's interface for adding a library path that should be searched.  */
enum ld_plugin_status
(*set_extra_library_path) (const char *path);

/* The linker's interface for issuing a warning or error message.  */
enum ld_plugin_status
(*message) (int level, const char *format, ...);

/* The linker's interface for retrieving the number of sections in an object.
   The handle is obtained in the claim_file handler.  This interface should
   only be invoked in the claim_file handler.   This function sets *COUNT to
   the number of sections in the object.  */
enum ld_plugin_status
(*get_input_section_count) (const void* handle, unsigned int *count);

/* The linker's interface for retrieving the section type of a specific
   section in an object.  This interface should only be invoked in the
   claim_file handler.  This function sets *TYPE to an ELF SHT_xxx value.  */
enum ld_plugin_status
(*get_input_section_type) (const struct ld_plugin_section section,
                                     unsigned int *type);

/* The linker's interface for retrieving the name of a specific section in
   an object. This interface should only be invoked in the claim_file handler.
   This function sets *SECTION_NAME_PTR to a null-terminated buffer allocated
   by malloc.  The plugin must free *SECTION_NAME_PTR.  */
enum ld_plugin_status
(*get_input_section_name) (const struct ld_plugin_section section,
                                     char **section_name_ptr);

/* The linker's interface for retrieving the contents of a specific section
   in an object.  This interface should only be invoked in the claim_file
   handler.  This function sets *SECTION_CONTENTS to point to a buffer that is
   valid until clam_file handler returns.  It sets *LEN to the size of the
   buffer.  */
enum ld_plugin_status
(*get_input_section_contents) (const struct ld_plugin_section section,
                                         const unsigned char **section_contents,
                                         size_t* len);

/* The linker's interface for specifying the desired order of sections.
   The sections should be specifed using the array SECTION_LIST in the
   order in which they should appear in the final layout.  NUM_SECTIONS
   specifies the number of entries in each array.  This should be invoked
   in the all_symbols_read handler.  */
enum ld_plugin_status
(*update_section_order) (const struct ld_plugin_section *section_list,
				   unsigned int num_sections);

/* The linker's interface for specifying that reordering of sections is
   desired so that the linker can prepare for it.  This should be invoked
   before update_section_order, preferably in the claim_file handler.  */
enum ld_plugin_status
(*allow_section_ordering) (void);

/* The linker's interface for specifying that a subset of sections is
   to be mapped to a unique segment.  If the plugin wants to call
   unique_segment_for_sections, it must call this function from a
   claim_file_handler or when it is first loaded.  */
enum ld_plugin_status
(*allow_unique_segment_for_sections) (void);

/* The linker's interface for specifying that a specific set of sections
   must be mapped to a unique segment.  ELF segments do not have names
   and the NAME is used as the name of the newly created output section
   that is then placed in the unique PT_LOAD segment.  FLAGS is used to
   specify if any additional segment flags need to be set.  For instance,
   a specific segment flag can be set to identify this segment.  Unsetting
   segment flags that would be set by default is not possible.  The
   parameter SEGMENT_ALIGNMENT when non-zero will override the default.  */
enum ld_plugin_status
(*unique_segment_for_sections) (
    const char* segment_name,
    uint64_t segment_flags,
    uint64_t segment_alignment,
    const struct ld_plugin_section * section_list,
    unsigned int num_sections);

/* The linker's interface for retrieving the section alignment requirement
   of a specific section in an object.  This interface should only be invoked in the
   claim_file handler.  This function sets *ADDRALIGN to the ELF sh_addralign
   value of the input section.  */
enum ld_plugin_status
(*get_input_section_alignment) (const struct ld_plugin_section section,
                                          unsigned int *addralign);

/* The linker's interface for retrieving the section size of a specific section
   in an object.  This interface should only be invoked in the claim_file handler.
   This function sets *SECSIZE to the ELF sh_size
   value of the input section.  */
enum ld_plugin_status
(*get_input_section_size) (const struct ld_plugin_section section,
                                     uint64_t *secsize);

/* The linker's interface for getting the list of wrapped symbols using the
   --wrap option. This sets *NUM_SYMBOLS to number of wrapped symbols and
   *WRAP_SYMBOL_LIST to the list of wrapped symbols. */
enum ld_plugin_status
(*get_wrap_symbols) (uint64_t *num_symbols,
                               const char ***wrap_symbol_list);

/* Linker interfaces: hook registration. */

/* The linker's interface for registering the "claim file" handler.  */
enum ld_plugin_status
(*register_claim_file) (ld_plugin_claim_file_handler handler);
/* The linker's interface for registering the "all symbols read" handler.  */
enum ld_plugin_status
(*register_all_symbols_read) (
  ld_plugin_all_symbols_read_handler handler);
/* The linker's interface for registering the cleanup handler.  */
enum ld_plugin_status
(*register_cleanup) (ld_plugin_cleanup_handler handler);
/* The linker's interface for registering the "new_input" handler. This handler
   will be notified when a new input file has been added after the
   all_symbols_read event, allowing the plugin to, for example, set a unique
   segment for sections in plugin-generated input files. */
enum ld_plugin_status
(*register_new_input) (ld_plugin_new_input_handler handler);


};

extern struct linker linker[1]; /* HACK so we can do linker-> */

#endif
