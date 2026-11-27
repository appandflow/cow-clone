#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow-clone.h"

#define STRINGIFY_(value) #value
#define STRINGIFY(value) STRINGIFY_(value)
#ifdef COW_CLONE_VERSION
#define VERSION_STRING STRINGIFY(COW_CLONE_VERSION)
#else
#define VERSION_STRING "dev"
#endif
#ifdef COW_CLONE_TARGET
#define TARGET_STRING STRINGIFY(COW_CLONE_TARGET)
#else
#define TARGET_STRING "unknown"
#endif

static const char usage[] = "usage: cow-clone [--json] [--] <source> <destination> [<source> <destination>]...\n"
                            "       cow-clone [--json] --stdin   (NUL-separated source and destination pairs)\n";

void out_of_memory(void) {
  fputs("cow-clone: out of memory\n", stderr);
  exit(1);
}

static const char *reason_name(enum reason reason) {
  switch (reason) {
  case REASON_UNSUPPORTED:
    return "unsupported";
  case REASON_CROSS_DEVICE:
    return "cross-device";
  default:
    return "failed";
  }
}

static void json_string(const char *text) {
  putchar('"');
  for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
    if (*c == '"' || *c == '\\')
      printf("\\%c", *c);
    else if (*c < 0x20)
      printf("\\u%04x", *c);
    else
      putchar(*c);
  }
  putchar('"');
}

/* Reads NUL-terminated strings from stdin into one buffer; returns how many. */
static size_t read_stdin(char ***out) {
  size_t used = 0, capacity = 1 << 16;
  char *buffer = malloc(capacity);
  if (!buffer) out_of_memory();
  size_t got;
  while ((got = fread(buffer + used, 1, capacity - used, stdin)) > 0) {
    used += got;
    if (used == capacity) {
      capacity *= 2;
      buffer = realloc(buffer, capacity);
      if (!buffer) out_of_memory();
    }
  }
  if (used > 0 && buffer[used - 1] != '\0') {
    if (used == capacity) {
      buffer = realloc(buffer, capacity + 1);
      if (!buffer) out_of_memory();
    }
    buffer[used++] = '\0';
  }
  size_t count = 0;
  for (size_t i = 0; i < used; i++)
    if (buffer[i] == '\0') count++;
  char **strings = malloc((count ? count : 1) * sizeof *strings);
  if (!strings) out_of_memory();
  size_t index = 0;
  for (size_t start = 0, i = 0; i < used; i++)
    if (buffer[i] == '\0') {
      strings[index++] = buffer + start;
      start = i + 1;
    }
  *out = strings;
  return count;
}

int cow_clone_main(int argc, char **argv) {
  int json = 0, from_stdin = 0, i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strcmp(argv[i], "--json") == 0)
      json = 1;
    else if (strcmp(argv[i], "--stdin") == 0)
      from_stdin = 1;
    else if (strcmp(argv[i], "--version") == 0) {
      printf("%s (%s)\n", VERSION_STRING, TARGET_STRING);
      return 0;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      fputs(usage, stdout);
      return 0;
    } else {
      fprintf(stderr, "cow-clone: unknown option %s\n%s", argv[i], usage);
      return 2;
    }
  }
  char **paths = argv + i;
  size_t count = (size_t)(argc - i);
  if (from_stdin) {
    if (count) {
      fprintf(stderr, "cow-clone: --stdin takes no paths\n%s", usage);
      return 2;
    }
    count = read_stdin(&paths);
  }
  if (count == 0 || count % 2) {
    fprintf(stderr, "cow-clone: %s\n%s", count ? "a source has no destination" : "no paths", usage);
    return 2;
  }
  int status = 0;
  if (json) fputs("{\"results\":[", stdout);
  for (size_t pair = 0; pair < count; pair += 2) {
    struct failure failure = {REASON_NONE, "", "", NULL};
    int cloned = clone_pair(paths[pair], paths[pair + 1], &failure);
    if (!cloned) status = 1;
    if (json) {
      if (pair) putchar(',');
      if (cloned)
        fputs("{\"cloned\":true}", stdout);
      else {
        fputs("{\"cloned\":false,\"reason\":", stdout);
        json_string(reason_name(failure.reason));
        fputs(",\"code\":", stdout);
        json_string(failure.code);
        fputs(",\"message\":", stdout);
        json_string(failure.message);
        fputs(",\"path\":", stdout);
        json_string(failure.path ? failure.path : paths[pair + 1]);
        putchar('}');
      }
    } else if (!cloned)
      fprintf(stderr, "cow-clone: %s: %s (%s)\n", failure.path ? failure.path : paths[pair + 1], failure.message,
              failure.code);
    free(failure.path);
  }
  if (json) fputs("]}\n", stdout);
  return status;
}
