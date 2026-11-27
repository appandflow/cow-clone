#ifndef COW_CLONE_POSIX_H
#define COW_CLONE_POSIX_H

#include "cow-clone.h"

void set_failure(struct failure *failure, int error, enum reason reason, const char *path);

/* Removes `path` and everything below it without following symbolic links. */
void remove_tree(const char *path);

#include <sys/stat.h>

/* Clones the tree at `source` file by file with clone_regular_file, on several threads, recreating directories and
 * symbolic links with their modes and times. */
int walk_clone(const char *source, const char *destination, struct failure *failure);

enum file_step { STEP_SOURCE, STEP_DESTINATION, STEP_CLONE };

/* Clones the regular file `source` to `destination` with the mode and times in `st`. Runs with a umask of 0. On
 * failure returns -1 with errno set and *step naming the call that failed, and removes `destination` if it created
 * it. */
int clone_regular_file(const char *source, const char *destination, const struct stat *st, enum file_step *step);

/* The reason a failed clone call gives, from its errno. */
enum reason clone_reason(int error);

#endif
