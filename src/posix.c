#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "posix.h"

static const struct {
  int value;
  const char *name;
} errno_names[] = {
    {ENOTSUP, "ENOTSUP"},
#if EOPNOTSUPP != ENOTSUP
    {EOPNOTSUPP, "EOPNOTSUPP"},
#endif
    {EXDEV, "EXDEV"},         {EEXIST, "EEXIST"},     {ENOENT, "ENOENT"},   {EACCES, "EACCES"},
    {EPERM, "EPERM"},         {EINVAL, "EINVAL"},     {ENOTTY, "ENOTTY"},   {ENOSYS, "ENOSYS"},
    {ENOSPC, "ENOSPC"},       {EDQUOT, "EDQUOT"},     {EROFS, "EROFS"},     {ENOTDIR, "ENOTDIR"},
    {EISDIR, "EISDIR"},       {ELOOP, "ELOOP"},       {ENAMETOOLONG, "ENAMETOOLONG"},
    {EMFILE, "EMFILE"},       {ENFILE, "ENFILE"},     {EIO, "EIO"},         {EBUSY, "EBUSY"},
    {ETXTBSY, "ETXTBSY"},     {EMLINK, "EMLINK"},     {ENOMEM, "ENOMEM"},   {EBADF, "EBADF"},
};

void set_failure(struct failure *failure, int error, enum reason reason, const char *path) {
  failure->reason = reason;
  snprintf(failure->code, sizeof failure->code, "E%d", error);
  for (size_t i = 0; i < sizeof errno_names / sizeof *errno_names; i++)
    if (errno_names[i].value == error) {
      snprintf(failure->code, sizeof failure->code, "%s", errno_names[i].name);
      break;
    }
  snprintf(failure->message, sizeof failure->message, "%s", strerror(error));
  free(failure->path);
  failure->path = path ? strdup(path) : NULL;
}

static int remove_at(int directory, const char *name) {
  struct stat st;
  if (fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW)) return 0;
  if (!S_ISDIR(st.st_mode)) return unlinkat(directory, name, 0) == 0;
  fchmodat(directory, name, 0700, 0);
  int fd = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  DIR *entries = fd >= 0 ? fdopendir(fd) : NULL;
  if (entries) {
    /* Removing entries while reading a directory can make readdir skip some on APFS, so read it again until a pass
     * removes nothing. */
    int removed;
    do {
      removed = 0;
      rewinddir(entries);
      struct dirent *entry;
      while ((entry = readdir(entries))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (remove_at(dirfd(entries), entry->d_name)) removed = 1;
      }
    } while (removed);
    closedir(entries);
  } else if (fd >= 0)
    close(fd);
  return unlinkat(directory, name, AT_REMOVEDIR) == 0;
}

void remove_tree(const char *path) { remove_at(AT_FDCWD, path); }

int main(int argc, char **argv) { return cow_clone_main(argc, argv); }
