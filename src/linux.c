#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "posix.h"

#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif

int clone_regular_file(const char *source, const char *destination, const struct stat *st, enum file_step *step) {
  int from = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (from < 0) {
    *step = STEP_SOURCE;
    return -1;
  }
  int to = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, st->st_mode & 0777);
  if (to < 0) {
    int error = errno;
    close(from);
    *step = STEP_DESTINATION;
    errno = error;
    return -1;
  }
  struct timespec times[2] = {st->st_atim, st->st_mtim};
  *step = STEP_CLONE;
  int ok = ioctl(to, FICLONE, from) == 0;
  if (ok) *step = STEP_DESTINATION;
  /* open(2) applies the permission bits; setuid, setgid and sticky need fchmod. */
  if (ok && (st->st_mode & 07000)) ok = fchmod(to, st->st_mode & 07777) == 0;
  if (ok) ok = futimens(to, times) == 0;
  int error = errno;
  close(to);
  close(from);
  if (ok) return 0;
  unlink(destination);
  errno = error;
  return -1;
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
