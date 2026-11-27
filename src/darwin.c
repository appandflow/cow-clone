#include <errno.h>
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

int clone_file(int source, int directory, const char *name, int *clone_call) {
  if (fclonefileat(source, directory, name, 0) == 0) return 0;
  *clone_call = 1;
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
