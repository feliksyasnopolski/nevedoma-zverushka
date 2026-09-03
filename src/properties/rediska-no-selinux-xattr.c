#include <stddef.h>

/* Debian has no SELinux policy; bionic's creator otherwise aborts on this
 * expected operation.  This preload is used only by the genuine creator. */
int fsetxattr(int fd, const char* name, const void* value, size_t size, int flags) {
    (void)fd; (void)name; (void)value; (void)size; (void)flags;
    return 0;
}
