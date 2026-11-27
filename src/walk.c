#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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

#define MAX_THREADS 8

/* A file or directory to finish: its source path for files, its destination path, and its source's status. */
struct job {
  char *source;
  char *destination;
  struct stat st;
};

struct jobs {
  struct job *items;
  size_t count, capacity;
};

struct walker {
  struct failure *failure;
  struct jobs files, directories;
  int created, probed;
  /* Shared by the file threads. */
  pthread_mutex_t lock;
  size_t next;
  int failed;
};

static char *join(const char *directory, const char *name) {
  size_t length = strlen(directory), name_length = strlen(name);
  char *path = malloc(length + name_length + 2);
  if (!path) out_of_memory();
  memcpy(path, directory, length);
  path[length] = '/';
  memcpy(path + length + 1, name, name_length + 1);
  return path;
}

static char *copy(const char *text) {
  size_t length = strlen(text);
  char *path = malloc(length + 1);
  if (!path) out_of_memory();
  memcpy(path, text, length + 1);
  return path;
}

static void add(struct jobs *jobs, char *source, char *destination, const struct stat *st) {
  if (jobs->count == jobs->capacity) {
    jobs->capacity = jobs->capacity ? jobs->capacity * 2 : 1024;
    jobs->items = realloc(jobs->items, jobs->capacity * sizeof *jobs->items);
    if (!jobs->items) out_of_memory();
  }
  jobs->items[jobs->count++] = (struct job){source, destination, *st};
}

static void release(struct jobs *jobs) {
  for (size_t i = 0; i < jobs->count; i++) {
    free(jobs->items[i].source);
    free(jobs->items[i].destination);
  }
  free(jobs->items);
}

static int fail(struct walker *walker, int error, enum reason reason, const char *path) {
  set_failure(walker->failure, error, reason, path);
  return 0;
}

static int set_times(struct walker *walker, const char *path, const struct stat *st) {
  struct timespec times[2] = {ACCESS_TIME(*st), MODIFY_TIME(*st)};
  if (utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW)) return fail(walker, errno, REASON_FAILED, path);
  return 1;
}

static int clone_link(struct walker *walker, const char *source, const char *destination, const struct stat *st,
                      int depth) {
  /* st_size is the target's length on Linux and macOS, but some file systems report 0, so grow until it fits. */
  size_t capacity = st->st_size > 0 ? (size_t)st->st_size + 1 : 256;
  char *target;
  ssize_t length;
  for (;;) {
    target = malloc(capacity);
    if (!target) out_of_memory();
    length = readlink(source, target, capacity);
    if (length < 0) {
      int error = errno;
      free(target);
      return fail(walker, error, REASON_FAILED, source);
    }
    if ((size_t)length < capacity) break;
    free(target);
    capacity *= 2;
  }
  target[length] = '\0';
  int created = symlink(target, destination) == 0;
  int error = errno;
  free(target);
  if (!created) return fail(walker, error, REASON_FAILED, destination);
  if (depth == 0) walker->created = 1;
#ifdef __APPLE__
  /* macOS keeps a mode on symbolic links, which the umask of 0 would leave at 0777. */
  if (fchmodat(AT_FDCWD, destination, st->st_mode & 07777, AT_SYMLINK_NOFOLLOW))
    return fail(walker, errno, REASON_FAILED, destination);
#endif
  return set_times(walker, destination, st);
}

/* Creates the destination's directories and symbolic links and lists its files, which the threads clone after. */
static int plan(struct walker *walker, const char *source, const char *destination, int depth) {
  struct stat st;
  if (lstat(source, &st)) return fail(walker, errno, REASON_FAILED, source);
  if (S_ISREG(st.st_mode)) {
    if (walker->probed) {
      add(&walker->files, copy(source), copy(destination), &st);
      return 1;
    }
    /* The first file is cloned at once, so a file system that cannot clone fails before the rest is planned. */
    walker->probed = 1;
    enum file_step step;
    if (clone_regular_file(source, destination, &st, &step) == 0) return 1;
    int error = errno;
    return fail(walker, error, step == STEP_CLONE ? clone_reason(error) : REASON_FAILED,
                step == STEP_SOURCE ? source : destination);
  }
  if (S_ISLNK(st.st_mode)) return clone_link(walker, source, destination, &st, depth);
  if (!S_ISDIR(st.st_mode)) {
    fail(walker, EINVAL, REASON_FAILED, source);
    strcpy(walker->failure->code, "ESPECIAL");
    strcpy(walker->failure->message, "A FIFO, socket or device file cannot be cloned");
    return 0;
  }
  /* Writable until the files are in; the mode and times are set once the threads are done. */
  if (mkdir(destination, 0700)) return fail(walker, errno, REASON_FAILED, destination);
  if (depth == 0) walker->created = 1;
  add(&walker->directories, NULL, copy(destination), &st);
  int fd = open(source, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  DIR *entries = fd >= 0 ? fdopendir(fd) : NULL;
  if (!entries) {
    int error = errno;
    if (fd >= 0) close(fd);
    return fail(walker, error, REASON_FAILED, source);
  }
  int ok = 1;
  while (ok) {
    errno = 0;
    struct dirent *entry = readdir(entries);
    if (!entry) {
      if (errno) ok = fail(walker, errno, REASON_FAILED, source);
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char *from = join(source, entry->d_name);
    char *to = join(destination, entry->d_name);
    ok = plan(walker, from, to, depth + 1);
    free(from);
    free(to);
  }
  closedir(entries);
  return ok;
}

static void *clone_files(void *argument) {
  struct walker *walker = argument;
  for (;;) {
    pthread_mutex_lock(&walker->lock);
    size_t index = walker->failed ? walker->files.count : walker->next++;
    pthread_mutex_unlock(&walker->lock);
    if (index >= walker->files.count) return NULL;
    const struct job *job = &walker->files.items[index];
    enum file_step step;
    if (clone_regular_file(job->source, job->destination, &job->st, &step) == 0) continue;
    int error = errno;
    pthread_mutex_lock(&walker->lock);
    if (!walker->failed) {
      walker->failed = 1;
      fail(walker, error, step == STEP_CLONE ? clone_reason(error) : REASON_FAILED,
           step == STEP_SOURCE ? job->source : job->destination);
    }
    pthread_mutex_unlock(&walker->lock);
  }
}

static size_t thread_count(size_t files) {
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  const char *setting = getenv("COW_CLONE_THREADS");
  if (setting && atoi(setting) > 0) count = atoi(setting);
  if (count < 1) count = 1;
  if (count > MAX_THREADS) count = MAX_THREADS;
  /* A thread start costs more than cloning a few files. */
  if ((size_t)count > files / 64 + 1) count = (long)(files / 64 + 1);
  return (size_t)count;
}

static int finish_directories(struct walker *walker) {
  /* Deepest first: setting a directory's mode or times after its parent's would change the parent's times. */
  for (size_t i = walker->directories.count; i-- > 0;) {
    const struct job *job = &walker->directories.items[i];
    if (chmod(job->destination, job->st.st_mode & 07777))
      return fail(walker, errno, REASON_FAILED, job->destination);
    if (!set_times(walker, job->destination, &job->st)) return 0;
  }
  return 1;
}

int walk_clone(const char *source, const char *destination, struct failure *failure) {
  struct walker walker = {.failure = failure};
  pthread_mutex_init(&walker.lock, NULL);
  mode_t mask = umask(0);
  int ok = plan(&walker, source, destination, 0);
  if (ok && walker.files.count) {
    size_t count = thread_count(walker.files.count);
    pthread_t threads[MAX_THREADS];
    size_t started = 0;
    while (started + 1 < count && pthread_create(&threads[started], NULL, clone_files, &walker) == 0) started++;
    clone_files(&walker);
    for (size_t i = 0; i < started; i++) pthread_join(threads[i], NULL);
    ok = !walker.failed;
  }
  if (ok) ok = finish_directories(&walker);
  umask(mask);
  if (!ok && walker.created) remove_tree(destination);
  release(&walker.files);
  release(&walker.directories);
  pthread_mutex_destroy(&walker.lock);
  return ok;
}
