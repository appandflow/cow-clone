#ifndef COW_CLONE_POSIX_H
#define COW_CLONE_POSIX_H

#include "cow-clone.h"

void set_failure(struct failure *failure, int error, enum reason reason, const char *path);

/* Removes `path` and everything below it without following symbolic links. */
void remove_tree(const char *path);

/* Clones the tree at `source` one file at a time with clone_file, recreating directories and symbolic links with
 * their modes and times. */
int walk_clone(const char *source, const char *destination, struct failure *failure);

/* Creates `name` in `directory` as a clone of the open regular file `source`. On failure returns -1 with errno set,
 * and sets *clone_call when the clone itself failed rather than creating the file. */
int clone_file(int source, int directory, const char *name, int *clone_call);

/* The reason a failed clone call gives, from its errno. */
enum reason clone_reason(int error);

#endif
