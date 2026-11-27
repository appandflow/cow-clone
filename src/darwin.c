#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/attr.h>
#include <sys/clonefile.h>
#include <sys/stat.h>

#include "posix.h"

/* clonefile(2): ENOTSUP when the volume does not support clones, EXDEV across volumes. */
enum reason clone_reason(int error) {
  if (error == EXDEV) return REASON_CROSS_DEVICE;
  if (error == ENOTSUP) return REASON_UNSUPPORTED;
  return REASON_FAILED;
}

int clone_regular_file(const char *source, const char *destination, const struct stat *st, enum file_step *step) {
  (void)st;
  /* Opened first, as on Linux, so the walker build fails on an unreadable file the same way. fclonefileat(2) keeps
   * the mode and times itself. */
  int from = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (from < 0) {
    *step = STEP_SOURCE;
    return -1;
  }
  int ok = fclonefileat(from, AT_FDCWD, destination, 0) == 0;
  int error = errno;
  close(from);
  if (ok) return 0;
  *step = error == EEXIST ? STEP_DESTINATION : STEP_CLONE;
  errno = error;
  return -1;
}

#ifdef COW_CLONE_WALK
/* A test build: clones file by file through the walker Linux uses, so its handling of directories, symbolic links,
 * modes, times and clean-up runs on macOS too. */
int clone_pair(const char *source, const char *destination, struct failure *failure) {
  return walk_clone(source, destination, failure);
}
#else
int clone_pair(const char *source, const char *destination, struct failure *failure) {
  struct stat st;
  if (lstat(destination, &st) == 0) {
    set_failure(failure, EEXIST, REASON_FAILED, destination);
    return 0;
  }
  /* One call clones a whole directory hierarchy on APFS. CLONE_NOFOLLOW clones a symbolic link itself. */
  if (clonefile(source, destination, CLONE_NOFOLLOW) == 0) return 1;
  int error = errno;
  int source_missing = error == ENOENT && lstat(source, &st) != 0;
  if (error != EEXIST) remove_tree(destination);
  set_failure(failure, error, clone_reason(error), source_missing ? source : destination);
  return 0;
}
#endif
