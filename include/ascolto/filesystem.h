#ifndef ASCOLTO_FILESYSTEM_H
#define ASCOLTO_FILESYSTEM_H

/* Create missing directories, preserving existing paths and permissions. */
int ascolto_mkdir_p(const char *directory);
int ascolto_mkdir_parent(const char *path);
#endif
