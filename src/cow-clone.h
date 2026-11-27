#ifndef COW_CLONE_H
#define COW_CLONE_H

enum reason { REASON_NONE, REASON_UNSUPPORTED, REASON_CROSS_DEVICE, REASON_FAILED };

struct failure {
  enum reason reason;
  /* An errno name such as ENOTSUP, or a Windows error mapped to one. */
  char code[32];
  char message[256];
  /* The path the failing call was about, in UTF-8; malloc'd, or NULL. */
  char *path;
};

/* Clones `source` to `destination`, which must not exist. Returns 1 when it cloned, 0 when it filled `failure`. When
 * a clone fails after it created `destination`, it removes what it created. */
int clone_pair(const char *source, const char *destination, struct failure *failure);

/* Platform entry point shared by every build. */
int cow_clone_main(int argc, char **argv);

void out_of_memory(void);

#endif
