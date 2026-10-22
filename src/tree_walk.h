#ifndef BFPILOT_TREE_WALK_H
#define BFPILOT_TREE_WALK_H
#include <sys/stat.h>

/* Visits regular files, directories and link metadata without following links.
 * Paths and frames live on the heap; directory identities and depth are bounded.
 * The callback runs once per entry. Return a negative errno to stop. */
int bfpilot_tree_walk(const char *path, dev_t device,
    int (*entry)(const char *path, const char *relative,
                 const struct stat *st, void *context),
    int (*cancelled)(void *context), void *context);
/* Directory callbacks run after their children and after closing their fd. */
int bfpilot_tree_walk_post(const char *path, dev_t device,
    int (*entry)(const char *path, const char *relative,
                 const struct stat *st, void *context),
    int (*cancelled)(void *context), void *context);
#endif
