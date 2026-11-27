#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "posix.h"

#ifdef __APPLE__
#define ACCESS_TIME(st) ((st).st_atimespec)
#define MODIFY_TIME(st) ((st).st_mtimespec)
#else
#define ACCESS_TIME(st) ((st).st_atim)
#define MODIFY_TIME(st) ((st).st_mtim)
#endif

struct path {
  char *text;
  size_t length, capacity;
};

struct walker {
  struct failure *failure;
  struct path source, destination;
  int created;
};

static void path_init(struct path *path, const char *text) {
  path->length = strlen(text);
  path->capacity = path->length + 256;
  path->text = malloc(path->capacity);
  if (!path->text) out_of_memory();
  memcpy(path->text, text, path->length + 1);
}

static size_t path_push(struct path *path, const char *name) {
  size_t before = path->length, length = strlen(name);
  if (before + length + 2 > path->capacity) {
    path->capacity = (before + length + 2) * 2;
    path->text = realloc(path->text, path->capacity);
    if (!path->text) out_of_memory();
  }
  path->text[before] = '/';
  memcpy(path->text + before + 1, name, length + 1);
  path->length = before + length + 1;
  return before;
}

static void path_pop(struct path *path, size_t before) {
  path->length = before;
  path->text[before] = '\0';
}

static int fail(struct walker *walker, int error, enum reason reason, const struct path *path) {
  set_failure(walker->failure, error, reason, path->text);
  return 0;
}

static int set_times(struct walker *walker, int directory, const char *name, const struct stat *st) {
  struct timespec times[2] = {ACCESS_TIME(*st), MODIFY_TIME(*st)};
  if (utimensat(directory, name, times, AT_SYMLINK_NOFOLLOW)) return fail(walker, errno, REASON_FAILED, &walker->destination);
  return 1;
}

static int clone_entry(struct walker *walker, int source_directory, const char *source_name, int directory,
                       const char *name, int depth);

static int clone_directory(struct walker *walker, int source_directory, const char *source_name, int directory,
                           const char *name, const struct stat *st, int depth) {
  if (mkdirat(directory, name, 0700)) return fail(walker, errno, REASON_FAILED, &walker->destination);
  if (depth == 0) walker->created = 1;
  int source = openat(source_directory, source_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (source < 0) return fail(walker, errno, REASON_FAILED, &walker->source);
  DIR *entries = fdopendir(source);
  if (!entries) {
    int error = errno;
    close(source);
    return fail(walker, error, REASON_FAILED, &walker->source);
  }
  int target = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (target < 0) {
    int error = errno;
    closedir(entries);
    return fail(walker, error, REASON_FAILED, &walker->destination);
  }
  int ok = 1;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(entries);
    if (!entry) {
      if (errno) ok = fail(walker, errno, REASON_FAILED, &walker->source);
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    size_t source_before = path_push(&walker->source, entry->d_name);
    size_t before = path_push(&walker->destination, entry->d_name);
    ok = clone_entry(walker, dirfd(entries), entry->d_name, target, entry->d_name, depth + 1);
    if (!ok) break;
    path_pop(&walker->source, source_before);
    path_pop(&walker->destination, before);
  }
  closedir(entries);
  if (ok && fchmod(target, st->st_mode & 07777)) ok = fail(walker, errno, REASON_FAILED, &walker->destination);
  if (ok) {
    /* Written last: creating the entries above changed the directory's times. */
    struct timespec times[2] = {ACCESS_TIME(*st), MODIFY_TIME(*st)};
    if (futimens(target, times)) ok = fail(walker, errno, REASON_FAILED, &walker->destination);
  }
  close(target);
  return ok;
}

static int clone_link(struct walker *walker, int source_directory, const char *source_name, int directory,
                      const char *name, const struct stat *st, int depth) {
  /* st_size is the target's length on Linux and macOS, but some file systems report 0, so grow until it fits. */
  size_t capacity = st->st_size > 0 ? (size_t)st->st_size + 1 : 256;
  char *target;
  ssize_t length;
  for (;;) {
    target = malloc(capacity);
    if (!target) out_of_memory();
    length = readlinkat(source_directory, source_name, target, capacity);
    if (length < 0) {
      int error = errno;
      free(target);
      return fail(walker, error, REASON_FAILED, &walker->source);
    }
    if ((size_t)length < capacity) break;
    free(target);
    capacity *= 2;
  }
  target[length] = '\0';
  int created = symlinkat(target, directory, name) == 0;
  int error = errno;
  free(target);
  if (!created) return fail(walker, error, REASON_FAILED, &walker->destination);
  if (depth == 0) walker->created = 1;
  return set_times(walker, directory, name, st);
}

static int clone_regular(struct walker *walker, int source_directory, const char *source_name, int directory,
                         const char *name, const struct stat *st, int depth) {
  int source = openat(source_directory, source_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (source < 0) return fail(walker, errno, REASON_FAILED, &walker->source);
  int clone_call = 0;
  int cloned = clone_file(source, directory, name, &clone_call) == 0;
  int error = errno;
  close(source);
  if (depth == 0 && (cloned || clone_call)) walker->created = 1;
  if (!cloned)
    return fail(walker, error, clone_call ? clone_reason(error) : REASON_FAILED, &walker->destination);
  if (fchmodat(directory, name, st->st_mode & 07777, 0)) return fail(walker, errno, REASON_FAILED, &walker->destination);
  return set_times(walker, directory, name, st);
}

static int clone_entry(struct walker *walker, int source_directory, const char *source_name, int directory,
                       const char *name, int depth) {
  struct stat st;
  if (fstatat(source_directory, source_name, &st, AT_SYMLINK_NOFOLLOW))
    return fail(walker, errno, REASON_FAILED, &walker->source);
  if (S_ISREG(st.st_mode)) return clone_regular(walker, source_directory, source_name, directory, name, &st, depth);
  if (S_ISDIR(st.st_mode)) return clone_directory(walker, source_directory, source_name, directory, name, &st, depth);
  if (S_ISLNK(st.st_mode)) return clone_link(walker, source_directory, source_name, directory, name, &st, depth);
  fail(walker, EINVAL, REASON_FAILED, &walker->source);
  strcpy(walker->failure->code, "ESPECIAL");
  strcpy(walker->failure->message, "A FIFO, socket or device file cannot be cloned");
  return 0;
}

int walk_clone(const char *source, const char *destination, struct failure *failure) {
  struct walker walker = {failure, {0}, {0}, 0};
  path_init(&walker.source, source);
  path_init(&walker.destination, destination);
  int ok = clone_entry(&walker, AT_FDCWD, source, AT_FDCWD, destination, 0);
  if (!ok && walker.created) remove_tree(destination);
  free(walker.source.text);
  free(walker.destination.text);
  return ok;
}
