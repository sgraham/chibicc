#include "dyibicc.h"

#ifdef _WIN64
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#endif

IMPLSTATIC char* bumpstrndup(const char* s, size_t n, AllocLifetime lifetime) {
  size_t l = strnlen(s, n);
  char* d = bumpcalloc(1, l + 1, lifetime);
  if (!d)
    return NULL;
  memcpy(d, s, l);
  d[l] = 0;
  return d;
}

IMPLSTATIC char* bumpstrdup(const char* s, AllocLifetime lifetime) {
  size_t l = strlen(s);
  char* d = bumpcalloc(1, l + 1, lifetime);
  if (!d)
    return NULL;
  memcpy(d, s, l);
  d[l] = 0;
  return d;
}

IMPLSTATIC char* dirname(char* s) {
  size_t i;
  if (!s || !*s)
    return ".";
  i = strlen(s) - 1;
  for (; s[i] == '/' || s[i] == '\\'; i--)
    if (!i)
      return "/";
  for (; s[i] != '/' || s[i] == '\\'; i--)
    if (!i)
      return ".";
  for (; s[i] == '/' || s[i] == '\\'; i--)
    if (!i)
      return "/";
  s[i + 1] = 0;
  return s;
}

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
IMPLSTATIC uint64_t align_to_u(uint64_t n, uint64_t align) {
  return (n + align - 1) / align * align;
}

IMPLSTATIC int64_t align_to_s(int64_t n, int64_t align) {
  return (n + align - 1) / align * align;
}

IMPLSTATIC unsigned int get_page_size(void) {
#if X64WIN
  SYSTEM_INFO system_info;
  GetSystemInfo(&system_info);
  return system_info.dwPageSize;
#else
  return sysconf(_SC_PAGESIZE);
#endif
}

IMPLSTATIC void strarray_push(StringArray* arr, char* s, AllocLifetime lifetime) {
  if (!arr->data) {
    arr->data = bumpcalloc(8, sizeof(char*), lifetime);
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    arr->data = bumplamerealloc(arr->data, sizeof(char*) * arr->capacity,
                                sizeof(char*) * arr->capacity * 2, lifetime);
    arr->capacity *= 2;
    for (int i = arr->len; i < arr->capacity; i++)
      arr->data[i] = NULL;
  }

  arr->data[arr->len++] = s;
}

IMPLSTATIC void strintarray_push(StringIntArray* arr, StringInt item, AllocLifetime lifetime) {
  if (!arr->data) {
    arr->data = bumpcalloc(8, sizeof(StringInt), lifetime);
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    arr->data = bumplamerealloc(arr->data, sizeof(StringInt) * arr->capacity,
                                sizeof(StringInt) * arr->capacity * 2, lifetime);
    arr->capacity *= 2;
    for (int i = arr->len; i < arr->capacity; i++)
      arr->data[i] = (StringInt){NULL, -1};
  }

  arr->data[arr->len++] = item;
}

// Returns the contents of a given file. Doesn't support '-' for reading from
// stdin.
IMPLSTATIC char* read_file_wrap_user(char* path, AllocLifetime lifetime) {
  char* contents;
  size_t size;
  if (!user_context->load_file_contents(path, &contents, &size))
    return NULL;

  char* buf = bumpcalloc(1, size + 1, lifetime);  // TODO: doesn't really need a calloc
  memcpy(buf, contents, size);
  free(contents);
  buf[size] = 0;
  return buf;
}

// Takes a printf-style format string and returns a formatted string.
IMPLSTATIC char* format(AllocLifetime lifetime, char* fmt, ...) {
  char buf[4096];

  va_list ap;
  va_start(ap, fmt);
  vsprintf(buf, fmt, ap);
  va_end(ap);
  return bumpstrdup(buf, lifetime);
}

IMPLSTATIC int outaf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = user_context->output_function(fmt, ap);
  va_end(ap);
  return ret;
}

#define ANSI_WHITE "\033[1;37m"
#define ANSI_GREEN "\033[1;32m"
#define ANSI_RED "\033[1;31m"
#define ANSI_RESET "\033[0m"

// Reports an error message in the following format.
//
// foo.c:10: x = y + 1;
//               ^ <error message here>
static void verror_at(char* filename, char* input, int line_no, char* loc, char* fmt, va_list ap) {
  // Find a line containing `loc`.
  char* line = loc;
  while (input < line && line[-1] != '\n')
    line--;

  char* end = loc;
  while (*end && *end != '\n')
    end++;

  // Print out the line.
  if (user_context->use_ansi_codes)
    outaf(ANSI_WHITE);

  int indent = outaf("%s:%d: ", filename, line_no);

  if (user_context->use_ansi_codes)
    outaf(ANSI_RESET);

  outaf("%.*s\n", (int)(end - line), line);

  // Show the error message.
  int pos = display_width(line, (int)(loc - line)) + indent;

  outaf("%*s", pos, "");  // print pos spaces.

  if (user_context->use_ansi_codes)
    outaf("%s^ %serror: %s", ANSI_GREEN, ANSI_RED, ANSI_WHITE);
  else
    outaf("^ error: ");

  user_context->output_function(fmt, ap);

  outaf("\n");
  if (user_context->use_ansi_codes)
    outaf("%s", ANSI_RESET);
}

IMPLSTATIC void error_at(char* loc, char* fmt, ...) {
  File* cf = compiler_state.tokenize__current_file;

  int line_no = 1;
  for (char* p = cf->contents; p < loc; p++)
    if (*p == '\n')
      line_no++;

  va_list ap;
  va_start(ap, fmt);
  verror_at(cf->name, cf->contents, line_no, loc, fmt, ap);
  longjmp(toplevel_update_jmpbuf, 1);
}

IMPLSTATIC void error_tok(Token* tok, char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt, ap);
  longjmp(toplevel_update_jmpbuf, 1);
}

IMPLSTATIC void warn_tok(Token* tok, char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt, ap);
  va_end(ap);
}

// Reports an error and exit update.
IMPLSTATIC void error(char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (!user_context || !user_context->output_function) {
    vfprintf(stderr, fmt, ap);
  } else {
    user_context->output_function(fmt, ap);
    outaf("\n");
  }
  longjmp(toplevel_update_jmpbuf, 1);
}

IMPLSTATIC void error_internal(char* file, int line, char* msg) {
  outaf("%sinternal error at %s:%d: %s%s\n%s", ANSI_RED, file, line, ANSI_WHITE, msg, ANSI_RESET);
  longjmp(toplevel_update_jmpbuf, 1);
}

#if X64WIN
IMPLSTATIC void register_function_table_data(UserContext* ctx, int func_count, char* base_addr) {
  if (!RtlAddFunctionTable((RUNTIME_FUNCTION*)ctx->function_table_data, func_count,
                           (DWORD64)base_addr)) {
    error("failed to RtlAddFunctionTable");
  }
}

IMPLSTATIC void unregister_and_free_function_table_data(UserContext* ctx) {
  if (ctx->function_table_data) {
    if (!RtlDeleteFunctionTable((RUNTIME_FUNCTION*)ctx->function_table_data)) {
      error("failed to RtlDeleteFunctionTable");
    }
    free(ctx->function_table_data);
    ctx->function_table_data = NULL;
  }
}
#endif
