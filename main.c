﻿// Notes and todos
// ---------------
//
// Windows x64 .pdata generation:
//
//   Need to RtlAddFunctionTable() so that even minimal stackwalking in
//   Disassembly view is correct in VS. Required for SEH too. cl /Fa emits
//   without using any helper macros for samples.
//
// Break up into small symbol-sized 'sections':
//
//   In order to update code without losing global state, need to be able to
//   replace and relink. Right now, link_dyos() does all the allocation of
//   global data at the same time as mapping the code in to executable pages.
//
//   The simplest fix would be to keep the mappings of globals around and not
//   reallocate them on updates (one map for global symbols, plus one per
//   translation unit). The code updating could still be tossing all code, and
//   relinking everything, but using the old hashmaps for data addresses.
//
//   Alternatively, it might be a better direction to break everything up into
//   symbol-sized chunks (i.e. either a variable or a function indexed by symbol
//   name). Initial and update become more similar, in that if any symbol is
//   updated, the old one (if any) gets thrown away, the new one gets mapped in
//   (whether code or data), and then everything that refers to it is patched.
//
//   The main gotchas that come to mind on the second approach are:
//
//     - The parser (and DynASM to assign labels) need to be initialized before
//     processing the whole file; C is just never going to be able to compile a
//     single function in isolation. So emit_data() and emit_text() need to make
//     sure that each symbol blob can be ripped out of the generated block, and
//     any offsets have to be saved relative to the start of that symbol for
//     emitting fixups. Probably codegen_pclabel() a start/end for each for
//     rippage/re-offseting.
//
//     - Need figure out how to name things. If everything becomes a flat bag of
//     symbols, we need to make sure that statics from a.c are local to a.c, so
//     they'll need to be file prefixed.
//
//     - Probably wll need to switch to a new format (some kv store or
//     something), as symbol-per-dyo would be a spamming of files to deal with.
//
// Testing for relinking:
//
//   Basic relinking is implemented, but there's no test driver that sequences a
//   bunch of code changes to make sure that the updates can be applied
//   successfully.
//
// khash <-> swisstable:
//
//   Look into hashtable libs, khash is used in link.c now and it seems ok, but
//   the interface isn't that pleasant. Possibly wrap and extern C a few common
//   instantiations of absl's with a more pleasant interface (and that could
//   replace hashmap.c too). Need to consider how they would/can integrate with
//   bumpalloc.
//
// Debugger:
//
//   Picking either ELF/DWARF or PE/COFF (and dropping .dyo) would probably be
//   the more practical way to get a better debugging experience, but then,
//   clang-win would also be a lot better. Tomorrow Corp demo for inspiration of
//   what needs to be implemented/included. Possibly still go with debug adapter
//   json thing (with extension messages?) so that an existing well-written UI
//   can be used.
//
// Improved codegen:
//
//   Bit of a black hole of effort and probably doesn't matter for a dev-focused
//   tool. But it would be easier to trace through asm if the data flow was less
//   hidden. Possibly basic use of otherwise-unused gp registers, possibly some
//   peephole, or higher level amalgamated instructions for codegen to use that
//   avoid the common cases of load/push, push/something/pop.
//
// Various "C+" language extensions:
//
//   Some possibilities:
//     - an import instead of #include that can be used when not touching system
//     stuff
//     - string type with syntax integration
//     - basic polymophic containers (dict, list, slice, sizedarray)
//     - range-based for loop (to go with containers)
//     - range notation
//
// rep stosb for local clear:
//
//   Especially on Windows where rdi is non-volatile, it seems like quite a lot
//   of instructions. At the very least we could only do one memset for all
//   locals to clear out a range.
//
// Don't emit __func__, __FUNCTION__ unless used:
//
//   Doesn't affect anything other than dyo size, but it bothers me seeing them
//   in there.
//
// Improve dumpdyo:
//
//   - Cross-reference the name to which fixups will be bound in disasm
//   - include dump as string for initializer bytes
//
// Implement TLS:
//
//   If needed.
//
// Implement inline ASM:
//
//   If needed.
//
// .dyo cache:
//
//   Based on compiler binary, "environment", and the contents of the .c file,
//   make a hash-based cache of dyos so that recompile can only build the
//   required files and relink while passing the whole module/program still.
//   Since there's no -D or other flags, "enviroment" could either be a hash of
//   all the files in the include search path, or alternatively hash after
//   preprocessing, or probably track all files include and include all of the
//   includes in the hash. Not overly important if total compile/link times
//   remain fast.
//
// In-memory dyo:
//
//   Alternatively to caching, maybe just save to a memory structure. Might be a
//   little faster for direct use, could still have a dump-dyo-from-mem for
//   debugging purposes. Goes more with an always-live compiler host hooked to
//   target.
//
// Consider merging some of the record types in dyo:
//
//     kTypeImport is (offset-to-fix, string-to-reference)
//     kTypeCodeReferenceToGlobal is (offset-to-fix, string-to-reference)
//     kTypeInitializerDataRelocation is (string-to-reference, addend)
//
//   The only difference between the first two is that one does GetProcAddress()
//   or similar, and the other looks in the export tables for other dyos. But we
//   might want data imported from host too.
//
//   The third is different in that the address to fix up is implicit because
//   it's in a sequence of data segment initializers, but just having all
//   imports be:
//
//      (offset-to-fix, string-to-reference, addend)
//
//   might be nicer.
//
//
#include "dyibicc.h"

#if X64WIN
#include <direct.h>
#endif

#define C(x) compiler_state.main__##x
#define L(x) linker_state.main__##x

static Token* must_tokenize_file(char* path) {
  Token* tok = tokenize_file(path);
  if (!tok)
    error("%s: %s", path, strerror(errno));
  return tok;
}

#if 0  // for -E call after preprocess().
static void print_tokens(Token* tok) {
  int line = 1;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (line > 1 && tok->at_bol)
      logout("\n");
    if (tok->has_space && !tok->at_bol)
      logout(" ");
    logout("%.*s", tok->len, tok->loc);
    line++;
  }
  logout("\n");
}
#endif

static int default_output_fn(int level, const char* fmt, va_list ap) {
  FILE* output_to = stdout;
  if (level >= 2)
    output_to = stderr;
  int ret = vfprintf(output_to, fmt, ap);
  return ret;
}

DyibiccContext* dyibicc_set_environment(DyibiccEnviromentData* env_data) {
  alloc_init(AL_Temp);

  // Clone env_data into allocated ctx

  size_t total_include_paths_len = 0;
  size_t num_include_paths = 0;
  for (const char** p = env_data->include_paths; *p; ++p) {
    total_include_paths_len += strlen(*p) + 1;
    ++num_include_paths;
  }

  StringArray sys_inc_paths = {0};
#if X64WIN
  strarray_push(&sys_inc_paths, format(AL_Temp, "%s/win", env_data->dyibicc_include_dir), AL_Temp);
  strarray_push(&sys_inc_paths, format(AL_Temp, "%s/all", env_data->dyibicc_include_dir), AL_Temp);

  strarray_push(&sys_inc_paths,
                "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt", AL_Temp);
  strarray_push(&sys_inc_paths,
                "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\um", AL_Temp);
  strarray_push(&sys_inc_paths,
                "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\shared",
                AL_Temp);
  strarray_push(&sys_inc_paths,
                "C:\\Program Files\\Microsoft Visual "
                "Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.34.31933\\include",
                AL_Temp);
#else
  strarray_push(&sys_inc_paths, format(AL_Temp, "%s/linux", env_data->dyibicc_include_dir),
                AL_Temp);
  strarray_push(&sys_inc_paths, format(AL_Temp, "%s/all", env_data->dyibicc_include_dir), AL_Temp);

  strarray_push(&sys_inc_paths, "/usr/local/include", AL_Temp);
  strarray_push(&sys_inc_paths, "/usr/include/x86_64-linux-gnu", AL_Temp);
  strarray_push(&sys_inc_paths, "/usr/include", AL_Temp);
#endif

  for (int i = 0; i < sys_inc_paths.len; ++i) {
    total_include_paths_len += strlen(sys_inc_paths.data[i]) + 1;
    ++num_include_paths;
  }

  StringArray dyo_output_paths = {0};
  size_t total_output_paths_len = 0;

  size_t total_source_files_len = 0;
  size_t num_files = 0;
  for (const char** p = env_data->files; *p; ++p) {
    total_source_files_len += strlen(*p) + 1;
    char* source_name_copy = bumpstrdup(*p, AL_Temp);
    for (char* q = source_name_copy; *q; ++q) {
      if (!isalnum(*q) && *q != '-' && *q != '_')
        *q = '@';
    }
    char* output_name = format(AL_Temp, "%s/%s.dyo", env_data->cache_dir, source_name_copy);
    strarray_push(&dyo_output_paths, output_name, AL_Temp);
    total_output_paths_len += strlen(output_name) + 1;

    ++num_files;
  }

#if X64WIN
  _mkdir(env_data->cache_dir);
#else
  mkdir(env_data->cache_dir, 0644);
#endif

  size_t entry_point_name_len =
      env_data->entry_point_name ? strlen(env_data->entry_point_name) + 1 : 0;
  if (!env_data->cache_dir) {
    env_data->cache_dir = ".";
  }
  size_t cache_dir_len = strlen(env_data->cache_dir) + 1;
  // Don't currently need dyibicc_include_dir once sys_inc_paths are added to.

  size_t total_size =
      sizeof(UserContext) +                       // base structure
      (num_include_paths * sizeof(char*)) +       // array in base structure
      (num_files * sizeof(DyoLinkData)) +         // array in base structure
      (total_include_paths_len * sizeof(char)) +  // pointed to by include_paths
      (total_source_files_len * sizeof(char)) +   // pointed to by DyoLinkData.source_name
      (total_output_paths_len * sizeof(char)) +   // pointed to by DyoLinkData.output_dyo_name
      entry_point_name_len +                      // two other strings
      cache_dir_len +                             //
      ((num_files + 1) * sizeof(HashMap));        // +1 beyond num_files for fully global dataseg

  UserContext* data = calloc(1, total_size);

  data->entry_point = NULL;
  data->get_function_address = env_data->get_function_address;
  data->output_function = env_data->output_function;
  if (!data->output_function) {
    data->output_function = default_output_fn;
  }

  char* d = (char*)(&data[1]);

  data->num_include_paths = num_include_paths;
  data->include_paths = (char**)d;
  d += sizeof(char*) * num_include_paths;

  data->num_files = num_files;
  data->files = (DyoLinkData*)d;
  d += sizeof(DyoLinkData) * num_files;

  data->global_data = (HashMap*)d;
  d += sizeof(HashMap) * (num_files + 1);

  if (env_data->entry_point_name) {
    data->entry_point_name = d;
    strcpy(d, env_data->entry_point_name);
    d += strlen(env_data->entry_point_name) + 1;
  }

  if (env_data->cache_dir) {
    data->cache_dir = d;
    strcpy(d, env_data->cache_dir);
    d += strlen(env_data->cache_dir) + 1;
  }

  int i = 0;
  for (const char** p = env_data->include_paths; *p; ++p) {
    data->include_paths[i++] = d;
    strcpy(d, *p);
    d += strlen(*p) + 1;
  }
  for (int j = 0; j < sys_inc_paths.len; ++j) {
    data->include_paths[i++] = d;
    strcpy(d, sys_inc_paths.data[j]);
    d += strlen(sys_inc_paths.data[j]) + 1;
  }

  i = 0;
  for (const char** p = env_data->files; *p; ++p) {
    DyoLinkData* dld = &data->files[i++];
    dld->source_name = d;
    strcpy(dld->source_name, *p);
    d += strlen(*p) + 1;
  }

  i = 0;
  for (int j = 0; j < dyo_output_paths.len; ++j) {
    DyoLinkData* dld = &data->files[i++];
    dld->output_dyo_name = d;
    strcpy(d, dyo_output_paths.data[j]);
    d += strlen(dyo_output_paths.data[j]) + 1;
  }

  for (size_t j = 0; j < num_files + 1; ++j) {
    // These maps store an arbitrary number of symbols, and they must persist
    // beyond AL_Link (to be saved for relink updates) so they must be manually
    // managed.
    data->global_data[j].alloc_lifetime = AL_Manual;
  }

  assert((size_t)(d - (char*)data) == total_size);

  alloc_reset(AL_Temp);

  user_context = data;
  return (DyibiccContext*)data;
}

#define TOMBSTONE ((void*)-1)
// These maps have keys strdup'd with AL_Manual, and values that are the data
// segment allocations allocated by aligned_allocate.
static void hashmap_custom_free(HashMap* map) {
  assert(map->alloc_lifetime == AL_Manual);
  for (int i = 0; i < map->capacity; i++) {
    HashEntry* ent = &map->buckets[i];
    if (ent->key && ent->key != TOMBSTONE) {
      alloc_free(ent->key, map->alloc_lifetime);
      aligned_free(ent->val);
    }
  }
  alloc_free(map->buckets, map->alloc_lifetime);
}

void dyibicc_free(DyibiccContext* context) {
  UserContext* ctx = (UserContext*)context;
  assert(ctx == user_context && "only one context currently supported");
  for (size_t i = 0; i < ctx->num_files + 1; ++i) {
    hashmap_custom_free(&ctx->global_data[i]);
  }
  free(ctx);
  user_context = NULL;
}

#define OPT_E 0

static FILE* open_file(char* path) {
  if (!path || strcmp(path, "-") == 0)
    return stdout;

  FILE* out = fopen(path, "wb");
  if (!out)
    error("cannot open output file: %s: %s", path, strerror(errno));
  return out;
}

bool dyibicc_update(DyibiccContext* context) {
  UserContext* ctx = (UserContext*)context;
  bool link_result = false;

  assert(ctx == user_context && "only one context currently supported");

  {
    for (size_t i = 0; i < ctx->num_files; ++i) {
      DyoLinkData* dld = &ctx->files[i];

      {
        alloc_init(AL_Compile);

        init_macros();
        C(base_file) = dld->source_name;
        Token* tok = must_tokenize_file(C(base_file));
        tok = preprocess(tok);

        codegen_init();  // Initializes dynasm so that parse() can assign labels.

        Obj* prog = parse(tok);
        FILE* dyo_out = open_file(dld->output_dyo_name);
        // TODO: need to figure out FILE vs longjmp, either will succeed or
        // error() which will leave this open. Probably the same for
        // preprocess().
        codegen(prog, dyo_out);
        fclose(dyo_out);

        alloc_reset(AL_Compile);
      }
    }

    {
      alloc_init(AL_Link);

      link_result = link_dyos();

      alloc_reset(AL_Link);
    }
  }

  return link_result;
}
