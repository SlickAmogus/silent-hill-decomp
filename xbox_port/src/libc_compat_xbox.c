/*
 * libc_compat_xbox.c - libc functions the decomp uses that pdclib (nxdk) lacks.
 * bzero is real; the POSIX file-I/O calls are stubs (no loose-file FS on Xbox
 * yet) that report failure so callers fall back to the CD/queue path.
 */
#include <string.h>   /* memset, size_t */

void bzero(void* p, size_t n) { memset(p, 0, n); }

int  open(const char* path, int flags)                { (void)path; (void)flags; return -1; }
int  close(int fd)                                    { (void)fd; return 0; }
long read(int fd, void* buf, unsigned long n)         { (void)fd; (void)buf; (void)n; return -1; }
long write(int fd, const void* buf, unsigned long n)  { (void)fd; (void)buf; return (long)n; }
long lseek(int fd, long off, int whence)              { (void)fd; (void)off; (void)whence; return -1; }
