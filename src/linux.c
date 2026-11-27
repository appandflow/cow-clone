#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "posix.h"

#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif

int clone_file(int source, int directory, const char *name, int *clone_call) {
  int target = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (target < 0) return -1;
  if (ioctl(target, FICLONE, source)) {
    int error = errno;
    close(target);
    *clone_call = 1;
    errno = error;
    return -1;
  }
  return close(target);
}

/* ioctl_ficlone(2): EOPNOTSUPP when the file system cannot share extents (ext4, tmpfs), EINVAL when it cannot share
 * these files' ranges, EXDEV across mounts. ENOTTY and ENOSYS come from file systems and kernels without the ioctl. */
enum reason clone_reason(int error) {
  if (error == EXDEV) return REASON_CROSS_DEVICE;
  if (error == EOPNOTSUPP || error == ENOTSUP || error == EINVAL || error == ENOTTY || error == ENOSYS)
    return REASON_UNSUPPORTED;
  return REASON_FAILED;
}

int clone_pair(const char *source, const char *destination, struct failure *failure) {
  return walk_clone(source, destination, failure);
}
