/* Test-process-only Linux syscall interposition; never linked into zvec. */
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdint.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *root;
static const char *arm;
static const char *audit;
static const char *operation;
static const char *path_filter;
static long after = 1;
static int once;
static long calls;

__attribute__((constructor)) static void configure(void) {
  root = getenv("ZVEC_FAULT_ROOT");
  arm = getenv("ZVEC_FAULT_ARM");
  audit = getenv("ZVEC_FAULT_AUDIT");
  operation = getenv("ZVEC_FAULT_OPERATION");
  path_filter = getenv("ZVEC_FAULT_PATH");
  const char *value = getenv("ZVEC_FAULT_AFTER");
  if (value) after = strtol(value, NULL, 10);
  once = getenv("ZVEC_FAULT_ONCE") != NULL;
}

static int matches(const char *path) {
  return root && path && strncmp(path, root, strlen(root)) == 0 &&
         path[strlen(root)] == '/';
}

static void fd_path(int fd, char *path) {
  char link[64];
  snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  ssize_t size = readlink(link, path, PATH_MAX - 1);
  path[size < 0 ? 0 : size] = '\0';
}

static void record(const char *kind, const char *name, const char *path,
                   int error) {
  if (!audit || !matches(path)) return;
  int saved = errno;
  char line[PATH_MAX + 128];
  int count = snprintf(line, sizeof(line), "%s %s errno=%d %s\n", kind, name,
                       error, path);
  int fd =
      syscall(SYS_openat, AT_FDCWD, audit, O_WRONLY | O_APPEND | O_CREAT, 0600);
  if (fd < 0 || count < 0 || (size_t)count >= sizeof(line) ||
      syscall(SYS_write, fd, line, count) != count) {
    /* An unrecorded injection must fail the run, not look like no fault hit. */
    _exit(125);
  }
  syscall(SYS_close, fd);
  errno = saved;
}

static int inject(const char *name, const char *path) {
  if (!matches(path) || !operation || strcmp(operation, name) != 0 || !arm ||
      access(arm, F_OK) != 0 ||
      (path_filter && !strstr(path + strlen(root), path_filter)))
    return 0;
  long call = __atomic_add_fetch(&calls, 1, __ATOMIC_SEQ_CST);
  if (call < after || (once && call != after)) return 0;
  record("INJECT", name, path, EIO);
  errno = EIO;
  return 1;
}

#define WRITE_WRAPPER(name, signature, arguments)          \
  ssize_t name signature {                                 \
    ssize_t(*real_fn) signature = dlsym(RTLD_NEXT, #name); \
    char path[PATH_MAX];                                   \
    fd_path(fd, path);                                     \
    if (inject("write", path)) return -1;                  \
    ssize_t result = real_fn arguments;                    \
    if (result < 0) record("ERROR", #name, path, errno);   \
    if (result > 0 && arm && access(arm, F_OK) == 0)       \
      record("PASS", #name, path, 0);                      \
    return result;                                         \
  }

WRITE_WRAPPER(write, (int fd, const void *buf, size_t count), (fd, buf, count))
WRITE_WRAPPER(pwrite, (int fd, const void *buf, size_t count, off_t offset),
              (fd, buf, count, offset))
WRITE_WRAPPER(pwrite64, (int fd, const void *buf, size_t count, off64_t offset),
              (fd, buf, count, offset))

#define SYNC_WRAPPER(name)                               \
  int name(int fd) {                                     \
    int (*real_fn)(int) = dlsym(RTLD_NEXT, #name);       \
    char path[PATH_MAX];                                 \
    fd_path(fd, path);                                   \
    if (inject("sync", path)) return -1;                 \
    int result = real_fn(fd);                            \
    if (result < 0) record("ERROR", #name, path, errno); \
    return result;                                       \
  }
SYNC_WRAPPER(fsync)
SYNC_WRAPPER(fdatasync)

#define OPEN_WRAPPER(name)                                            \
  int name(const char *path, int flags, ...) {                        \
    int (*real_fn)(const char *, int, ...) = dlsym(RTLD_NEXT, #name); \
    mode_t mode = 0;                                                  \
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {      \
      va_list ap;                                                     \
      va_start(ap, flags);                                            \
      mode = va_arg(ap, int);                                         \
      va_end(ap);                                                     \
    }                                                                 \
    int result = real_fn(path, flags, mode);                          \
    if (result < 0) record("ERROR", #name, path, errno);              \
    return result;                                                    \
  }
OPEN_WRAPPER(open)
OPEN_WRAPPER(open64)

/* Resolve the file for an mmap flush without changing mapping lifetimes. */
int msync(void *address, size_t length, int flags) {
  int (*real_fn)(void *, size_t, int) = dlsym(RTLD_NEXT, "msync");
  char path[PATH_MAX] = "";
  if (operation && strcmp(operation, "msync") == 0 && arm &&
      access(arm, F_OK) == 0) {
    int saved = errno;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) _exit(125);
    char line[PATH_MAX + 256];
    while (fgets(line, sizeof(line), maps)) {
      unsigned long begin, end;
      int offset = 0;
      if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &begin, &end,
                 &offset) == 2 && offset > 0 &&
          (uintptr_t)address >= begin && (uintptr_t)address < end) {
        size_t size = strcspn(line + offset, "\n");
        if (size >= sizeof(path)) _exit(125);
        memcpy(path, line + offset, size);
        path[size] = '\0';
        break;
      }
    }
    if (ferror(maps)) _exit(125);
    fclose(maps);
    errno = saved;
    if (inject("msync", path)) return -1;
  }
  int result = real_fn(address, length, flags);
  if (result < 0) record("ERROR", "msync", path, errno);
  return result;
}

int mkdir(const char *path, mode_t mode) {
  int (*real_fn)(const char *, mode_t) = dlsym(RTLD_NEXT, "mkdir");
  int result = real_fn(path, mode);
  if (result < 0) record("ERROR", "mkdir", path, errno);
  return result;
}
