/* imports.c -- Android AArch64 core import resolution
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>
#include <locale.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <fenv.h>
#include <setjmp.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <switch.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <utime.h>
#include <zlib.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "pthr.h"
#include "hooks.h"
#include "dl_emu.h"
#include "opensles.h"
#include "drastic_renderer.h"
#include "drastic_vk_capture.h"

// crt/newlib-provided symbols forwarded by address
int *__errno(void);

// BSD/bionic legacy `_ctype_`: [0] is the EOF guard, [c+1] holds the mask for
// char c. Vendored libs in the core index it as `_ctype_[(c)+1]`. We provide our
// own (no dependence on / collision with newlib's symbol) and fill it from the C
// locale in update_imports() so isalpha/isdigit-via-table stay correct.
unsigned char g_ctype_table[1 + 256];

// stdout/stderr are DATA imports (FILE* variables); point them into fake_sF so
// the stdio shims recognise them. fake_sF is uint8_t[3][0x100] in libc_shim.c.
FILE *stdout_ptr = (FILE *)&fake_sF[1][0];
FILE *stderr_ptr = (FILE *)&fake_sF[2][0];

// ---------------------------------------------------------------------------
// small local shims (the long tail; the heavy lifting is in libc_shim/pthr/egl)
// ---------------------------------------------------------------------------

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  (void)tag;
  (void)fmt;
  return 0;
}

static int __android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio;
  (void)tag;
  (void)text;
  return 0;
}
static int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va) {
  (void)prio;
  (void)tag;
  (void)fmt;
  (void)va;
  return 0;
}

// sleeping/yielding is a GL-handover point (single-context: cheap)
static int nanosleep_park(const struct timespec *req, struct timespec *rem) {
  egl_gl_ownership_park();
  int r = nanosleep(req, rem);
  return r;
}

// pread isn't in libnx/newlib: emulate via save/seek/read/restore.
static ssize_t pread_compat(int fd, void *buf, size_t n, off_t off) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  ssize_t r = read(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}

// newlib/libnx gaps -> benign stubs / libnx-backed
static long getpagesize_fake(void) { return 0x1000; }
static int lockf_fake(int fd, int cmd, off_t len) { (void)fd; (void)cmd; (void)len; return 0; }
static int sigemptyset_fake(void *set) { if (set) memset(set, 0, sizeof(uint64_t)); return 0; }
// arc4random via libnx CSRNG (newlib's pulls getentropy/_getentropy_r, absent here)
static uint32_t arc4random_fake(void) { uint32_t v; randomGet(&v, sizeof(v)); return v; }
static void arc4random_buf_fake(void *buf, size_t n) { randomGet(buf, n); }

// fortified stdio/io: forward to the unchecked shim (we trust the core's sizes)
static size_t __fread_chk_fake(void *p, size_t pn, size_t sz, size_t n, FILE *f) {
  (void)pn; return fread_fake(p, sz, n, f);
}
static size_t __fwrite_chk_fake(const void *p, size_t pn, size_t sz, size_t n, FILE *f) {
  (void)pn; return fwrite_fake(p, sz, n, f);
}
static ssize_t __pread_chk_fake(int fd, void *buf, size_t count, off_t off, size_t bn) {
  (void)bn; return pread_compat(fd, buf, count, off);
}
static ssize_t __write_chk_fake(int fd, const void *buf, size_t count, size_t bn) {
  (void)bn; return write(fd, buf, count);
}

// locale ctype (the core only needs ASCII semantics)
static int tolower_l_fake(int c, locale_t l) { (void)l; return tolower(c); }
static int toupper_l_fake(int c, locale_t l) { (void)l; return toupper(c); }

// ioctl: ashmem fds get the ashmem ioctls (SET_NAME/SET_SIZE/GET_SIZE etc); the
// VTLB memory setup bails if SET_SIZE fails. Everything else -> failure.
static int ioctl_fake(int fd, unsigned long req, ...) {
  int ret;
  if (ashmem_ioctl(fd, req, &ret) == 0) return ret;
  errno = ENOTTY; return -1;
}

// Bionic uses bit 0 for TIMER_ABSTIME; newlib uses a different value.
static int clock_nanosleep_fake(clockid_t clk, int flags, const struct timespec *req,
                                struct timespec *rem) {
  struct timespec d = *req;
  if (flags & 1) { // Android TIMER_ABSTIME == 1: req is an absolute deadline
    struct timespec now;
    clock_gettime_fake(clk, &now);
    d.tv_sec = req->tv_sec - now.tv_sec;
    d.tv_nsec = req->tv_nsec - now.tv_nsec;
    if (d.tv_nsec < 0) { d.tv_sec--; d.tv_nsec += 1000000000L; }
    if (d.tv_sec < 0) return 0; // deadline already passed
  }
  if (d.tv_sec > 0 || d.tv_nsec > 33000000L) { // cap at ~2 frames (60fps)
    d.tv_sec = 0; d.tv_nsec = 33000000L;
  }
  return nanosleep(&d, rem);
}


// Linux-only syscalls newlib/libnx doesn't provide -> benign stubs.
static int prctl_fake(int option, ...) { (void)option; return 0; }
static int fallocate_fake(int fd, int mode, off_t off, off_t len) {
  (void)mode; (void)off; // best-effort: just ensure the file is at least len
  return ftruncate(fd, off + len);
}
static int sched_setaffinity_fake(int pid, size_t sz, const void *mask) {
  (void)pid; (void)sz; (void)mask; return 0; // core pins its own threads via libnx
}

static void NX_NORETURN core_terminate(const char *stage, int status) {
  (void)stage;
  (void)status;
  /* Terminate directly instead of walking the homebrew loader's exit hook.
   * The latter deliberately raises a user break and obscures the originating
   * core stack in Atmosphere reports. */
  svcExitProcess();
  for (;;) svcSleepThread(1000000000ULL);
}

static void NX_NORETURN abort_fake(void) {
  core_terminate("host-core-abort", 134);
}

static void NX_NORETURN stack_chk_fail_fake(void) {
  core_terminate("host-core-stack-check-failed", 127);
}

static char *__strncpy_chk_fake(void *dst, const void *src, size_t count,
                                size_t dst_size) {
  (void)dst_size;
  return strncpy((char *)dst, (const char *)src, count);
}
static char *__strrchr_chk_fake(const char *string, int value,
                                size_t string_size) {
  (void)string_size;
  return strrchr(string, value);
}
static int __umask_chk_fake(int mask) { (void)mask; return 0; }
static void *memalign_fake(size_t alignment, size_t size) {
  return memalign(alignment, size);
}
static FILE *popen_fake(const char *command, const char *mode) {
  (void)command; (void)mode; errno = ENOSYS; return NULL;
}
static int pclose_fake(FILE *file) { (void)file; errno = ENOSYS; return -1; }
static void *getpwnam_fake(const char *name) { (void)name; return NULL; }
static void *getgrnam_fake(const char *name) { (void)name; return NULL; }
static int no_path_operation(const char *a, ...) {
  (void)a; errno = ENOSYS; return -1;
}
static int flock_fake(int descriptor, int operation) {
  (void)descriptor; (void)operation; return 0;
}
static long random_fake(void) { return rand(); }
static void srandom_fake(unsigned seed) { srand(seed); }
static int pthread_attr_setdetachstate_fake(pthread_attr_t_bionic *attribute,
                                            int state) {
  (void)attribute; (void)state; return 0;
}
typedef struct {
  int64_t type, block_size;
  uint64_t blocks, blocks_free, blocks_available, files, files_free;
  int64_t fsid[2], name_length, fragment_size, flags;
  int64_t spare[4];
} BionicStatFs;
static int statfs_fake(const char *path, BionicStatFs *output) {
  if (!output) { errno = EFAULT; return -1; }
  struct statvfs status;
  if (statvfs(fix_path(path), &status) != 0) return -1;
  memset(output, 0, sizeof(*output));
  output->block_size = status.f_bsize;
  output->fragment_size = status.f_frsize;
  output->blocks = status.f_blocks;
  output->blocks_free = status.f_bfree;
  output->blocks_available = status.f_bavail;
  output->files = status.f_files;
  output->files_free = status.f_ffree;
  output->name_length = status.f_namemax;
  return 0;
}
static void exit_fake(int status) {
  core_terminate("host-core-exit", status);
}

static void perror_fake(const char *prefix) {
  (void)prefix;
}

static int c_isalnum(int c) { return isalnum(c); }
static int c_isalpha(int c) { return isalpha(c); }
static int c_iscntrl(int c) { return iscntrl(c); }
static int c_isgraph(int c) { return isgraph(c); }
static int c_islower(int c) { return islower(c); }
static int c_ispunct(int c) { return ispunct(c); }
static int c_isspace(int c) { return isspace(c); }
static int c_isupper(int c) { return isupper(c); }
static int c_isxdigit(int c) { return isxdigit(c); }
static int c_tolower(int c) { return tolower(c); }
static int c_toupper(int c) { return toupper(c); }

#define DRASTIC_GL_IMPORT(name) ((uintptr_t)&name)
#define DRASTIC_GL_BIND_TEXTURE_IMPORT ((uintptr_t)&glBindTexture)
#define DRASTIC_GL_TEX_SUB_IMAGE_IMPORT ((uintptr_t)&glTexSubImage2D)

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD },
  { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__umask_chk", (uintptr_t)&__umask_chk_fake },
  { "_longjmp", (uintptr_t)&longjmp },
  { "_setjmp", (uintptr_t)&setjmp },
  { "access", (uintptr_t)&access },
  { "asinf", (uintptr_t)&asinf },
  { "bsearch", (uintptr_t)&bsearch },
  { "chmod", (uintptr_t)&chmod },
  { "compress", (uintptr_t)&compress },
  { "compressBound", (uintptr_t)&compressBound },
  { "cosf", (uintptr_t)&cosf },
  { "coshf", (uintptr_t)&coshf },
  { "dlerror", (uintptr_t)&dlerror_fake },
  { "dup", (uintptr_t)&dup },
  { "exit", (uintptr_t)&exit_fake },
  { "expf", (uintptr_t)&expf },
  { "fgetc", (uintptr_t)&fgetc },
  { "flock", (uintptr_t)&flock_fake },
  { "flockfile", (uintptr_t)&flockfile },
  { "freopen", (uintptr_t)&freopen },
  { "frexpf", (uintptr_t)&frexpf },
  { "funlockfile", (uintptr_t)&funlockfile },
  { "getc_unlocked", (uintptr_t)&getc_unlocked },
  { "getgrnam", (uintptr_t)&getgrnam_fake },
  { "getopt_long", (uintptr_t)&getopt_long },
  { "getpwnam", (uintptr_t)&getpwnam_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "glActiveTexture", DRASTIC_GL_IMPORT(glActiveTexture) },
  { "glAttachShader", DRASTIC_GL_IMPORT(glAttachShader) },
  { "glBindFramebuffer", DRASTIC_GL_IMPORT(glBindFramebuffer) },
  { "glBindTexture", DRASTIC_GL_BIND_TEXTURE_IMPORT },
  { "glCheckFramebufferStatus", DRASTIC_GL_IMPORT(glCheckFramebufferStatus) },
  { "glClear", DRASTIC_GL_IMPORT(glClear) },
  { "glCompileShader", DRASTIC_GL_IMPORT(glCompileShader) },
  { "glCreateProgram", DRASTIC_GL_IMPORT(glCreateProgram) },
  { "glCreateShader", DRASTIC_GL_IMPORT(glCreateShader) },
  { "glDeleteFramebuffers", DRASTIC_GL_IMPORT(glDeleteFramebuffers) },
  { "glDeleteProgram", DRASTIC_GL_IMPORT(glDeleteProgram) },
  { "glDeleteShader", DRASTIC_GL_IMPORT(glDeleteShader) },
  { "glDeleteTextures", DRASTIC_GL_IMPORT(glDeleteTextures) },
  { "glDetachShader", DRASTIC_GL_IMPORT(glDetachShader) },
  { "glDrawArrays", DRASTIC_GL_IMPORT(glDrawArrays) },
  { "glEnableVertexAttribArray", DRASTIC_GL_IMPORT(glEnableVertexAttribArray) },
  { "glFinish", DRASTIC_GL_IMPORT(glFinish) },
  { "glFramebufferTexture2D", DRASTIC_GL_IMPORT(glFramebufferTexture2D) },
  { "glGenFramebuffers", DRASTIC_GL_IMPORT(glGenFramebuffers) },
  { "glGenTextures", DRASTIC_GL_IMPORT(glGenTextures) },
  { "glGetAttribLocation", DRASTIC_GL_IMPORT(glGetAttribLocation) },
  { "glGetShaderInfoLog", DRASTIC_GL_IMPORT(glGetShaderInfoLog) },
  { "glGetShaderiv", DRASTIC_GL_IMPORT(glGetShaderiv) },
  { "glGetUniformLocation", DRASTIC_GL_IMPORT(glGetUniformLocation) },
  { "glLinkProgram", DRASTIC_GL_IMPORT(glLinkProgram) },
  { "glShaderSource", DRASTIC_GL_IMPORT(glShaderSource) },
  { "glTexImage2D", DRASTIC_GL_IMPORT(glTexImage2D) },
  { "glTexParameteri", DRASTIC_GL_IMPORT(glTexParameteri) },
  { "glTexSubImage2D", DRASTIC_GL_TEX_SUB_IMAGE_IMPORT },
  { "glUniform1f", DRASTIC_GL_IMPORT(glUniform1f) },
  { "glUniform1i", DRASTIC_GL_IMPORT(glUniform1i) },
  { "glUniform2f", DRASTIC_GL_IMPORT(glUniform2f) },
  { "glUniform4f", DRASTIC_GL_IMPORT(glUniform4f) },
  { "glUseProgram", DRASTIC_GL_IMPORT(glUseProgram) },
  { "glVertexAttribPointer", DRASTIC_GL_IMPORT(glVertexAttribPointer) },
  { "glViewport", DRASTIC_GL_IMPORT(glViewport) },
  { "inflate", (uintptr_t)&inflate },
  { "inflateBack", (uintptr_t)&inflateBack },
  { "inflateBackEnd", (uintptr_t)&inflateBackEnd },
  { "inflateBackInit_", (uintptr_t)&inflateBackInit_ },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "isalnum", (uintptr_t)&c_isalnum },
  { "isalpha", (uintptr_t)&c_isalpha },
  { "iscntrl", (uintptr_t)&c_iscntrl },
  { "isgraph", (uintptr_t)&c_isgraph },
  { "islower", (uintptr_t)&c_islower },
  { "ispunct", (uintptr_t)&c_ispunct },
  { "isspace", (uintptr_t)&c_isspace },
  { "isupper", (uintptr_t)&c_isupper },
  { "isxdigit", (uintptr_t)&c_isxdigit },
  { "lchown", (uintptr_t)&no_path_operation },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "link", (uintptr_t)&no_path_operation },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "lstat", (uintptr_t)&stat_fake },
  { "memalign", (uintptr_t)&memalign_fake },
  { "memmem", (uintptr_t)&memmem },
  { "optarg", (uintptr_t)&optarg },
  { "pclose", (uintptr_t)&pclose_fake },
  { "perror", (uintptr_t)&perror_fake },
  { "popen", (uintptr_t)&popen_fake },
  { "powf", (uintptr_t)&powf },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_soloader },
  { "rand", (uintptr_t)&rand },
  { "random", (uintptr_t)&random_fake },
  { "read", (uintptr_t)&read },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "signal", (uintptr_t)&signal },
  { "sinf", (uintptr_t)&sinf },
  { "sinhf", (uintptr_t)&sinhf },
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  { "srandom", (uintptr_t)&srandom_fake },
  { "statfs", (uintptr_t)&statfs_fake },
  { "strcoll", (uintptr_t)&strcoll },
  { "strncat", (uintptr_t)&strncat },
  { "strspn", (uintptr_t)&strspn },
  { "symlink", (uintptr_t)&no_path_operation },
  { "tanf", (uintptr_t)&tanf },
  { "tanhf", (uintptr_t)&tanhf },
  { "tmpfile", (uintptr_t)&tmpfile },
  { "tolower", (uintptr_t)&c_tolower },
  { "toupper", (uintptr_t)&c_toupper },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "uncompress", (uintptr_t)&uncompress },
  { "ungetc", (uintptr_t)&ungetc },
  { "utime", (uintptr_t)&utime },
  { "vswprintf", (uintptr_t)&vswprintf },
  { "wcscat", (uintptr_t)&wcscat },
  { "wcschr", (uintptr_t)&wcschr },
  { "wcscmp", (uintptr_t)&wcscmp },
  { "wcscpy", (uintptr_t)&wcscpy },
  { "wcsncat", (uintptr_t)&wcsncat },
  { "wcsncmp", (uintptr_t)&wcsncmp },
  { "wcsncpy", (uintptr_t)&wcsncpy },
  { "wcspbrk", (uintptr_t)&wcspbrk },
  { "wcsrchr", (uintptr_t)&wcsrchr },
  { "write", (uintptr_t)&write },
  { "ASharedMemory_create", (uintptr_t)&ASharedMemory_create_fake },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint_fake },
  { "__android_log_write", (uintptr_t)&__android_log_write_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__fread_chk", (uintptr_t)&__fread_chk_fake },
  { "__fwrite_chk", (uintptr_t)&__fwrite_chk_fake },
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__open_2", (uintptr_t)&open2_fake },
  { "__pread_chk", (uintptr_t)&__pread_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&stack_chk_fail_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__write_chk", (uintptr_t)&__write_chk_fake },
  { "_ctype_", (uintptr_t)&g_ctype_table[0] },
  { "abort", (uintptr_t)&abort_fake },
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "arc4random", (uintptr_t)&arc4random_fake },
  { "arc4random_buf", (uintptr_t)&arc4random_buf_fake },
  { "asin", (uintptr_t)&asin },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "atoi", (uintptr_t)&atoi },
  { "btowc", (uintptr_t)&btowc },
  { "calloc", (uintptr_t)&calloc },
  { "clearerr", (uintptr_t)&clearerr },
  { "clock", (uintptr_t)&clock },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "clock_nanosleep", (uintptr_t)&clock_nanosleep_fake },
  { "close", (uintptr_t)&close_fake },
  { "closedir", (uintptr_t)&closedir },
  { "closelog", (uintptr_t)&ret0 },
  { "cos", (uintptr_t)&cos },
  { "ctime_r", (uintptr_t)&ctime_r },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "eglBindAPI", (uintptr_t)&eglBindAPIHook },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfigHook },
  { "eglCreateContext", (uintptr_t)&eglCreateContextHook },
  { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurfaceHook },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurfaceHook },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContextHook },
  { "eglDestroySurface", (uintptr_t)&eglDestroySurfaceHook },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglGetDisplay", (uintptr_t)&eglGetDisplayHook },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddressHook },
  { "eglInitialize", (uintptr_t)&eglInitializeHook },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrentHook },
  { "eglQueryString", (uintptr_t)&eglQueryStringHook },
  { "eglQuerySurface", (uintptr_t)&eglQuerySurfaceHook },
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffersHook },
  { "eglSwapInterval", (uintptr_t)&eglSwapIntervalHook },
  { "exp", (uintptr_t)&exp },
  { "exp2", (uintptr_t)&exp2 },
  { "fallocate", (uintptr_t)&fallocate_fake },
  { "fchmod", (uintptr_t)&fchmod },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "fegetround", (uintptr_t)&fegetround },
  { "feof", (uintptr_t)&feof },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fesetround", (uintptr_t)&fesetround },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgets", (uintptr_t)&fgets },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "free", (uintptr_t)&free },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "fstat", (uintptr_t)&fstat_fake },
  { "fstat64", (uintptr_t)&fstat_fake },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "getcwd", (uintptr_t)&getcwd },
  { "getenv", (uintptr_t)&getenv },
  { "getpagesize", (uintptr_t)&getpagesize_fake },
  { "getpid", (uintptr_t)&getpid },
  { "gettid", (uintptr_t)&gettid_fake },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "ioctl", (uintptr_t)&ioctl_fake },
  { "isatty", (uintptr_t)&isatty },
  { "isdigit_l", (uintptr_t)&isdigit_l },
  { "islower_l", (uintptr_t)&islower_l },
  { "isupper_l", (uintptr_t)&isupper_l },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "isxdigit_l", (uintptr_t)&isxdigit_l },
  { "localeconv", (uintptr_t)&localeconv },
  { "localtime", (uintptr_t)&localtime },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "lockf", (uintptr_t)&lockf_fake },
  { "log", (uintptr_t)&log },
  { "log2", (uintptr_t)&log2 },
  { "longjmp", (uintptr_t)&longjmp },
  { "lseek", (uintptr_t)&lseek },
  { "lseek64", (uintptr_t)&lseek },
  { "malloc", (uintptr_t)&malloc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy_jit_fake },
  { "memmove", (uintptr_t)&memmove_jit_fake },
  { "memset", (uintptr_t)&memset_jit_fake },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "mkstemp", (uintptr_t)&mkstemp },
  { "mktime", (uintptr_t)&mktime },
  { "mmap", (uintptr_t)&mmap_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "nanosleep", (uintptr_t)&nanosleep_park },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "open", (uintptr_t)&open_fake },
  { "opendir", (uintptr_t)&opendir },
  { "openlog", (uintptr_t)&ret0 },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "pow", (uintptr_t)&pow },
  { "prctl", (uintptr_t)&prctl_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_soloader },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_soloader },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_soloader },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_soloader },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_soloader },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_soloader },
  { "pthread_create", (uintptr_t)&pthread_create_soloader },
  { "pthread_detach", (uintptr_t)&pthread_detach_soloader },
  { "pthread_equal", (uintptr_t)&pthread_equal_soloader },
  { "pthread_getcpuclockid", (uintptr_t)&pthread_getcpuclockid },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_soloader },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_join", (uintptr_t)&pthread_join_soloader },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_soloader },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_soloader },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_soloader },
  { "pthread_once", (uintptr_t)&pthread_once_soloader },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock },
  { "pthread_self", (uintptr_t)&pthread_self_soloader },
  { "pthread_setschedparam", (uintptr_t)&ret0 },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "qsort", (uintptr_t)&qsort },
  { "readdir", (uintptr_t)&readdir_fake },
  { "realloc", (uintptr_t)&realloc },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "rmdir", (uintptr_t)&rmdir },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_setaffinity", (uintptr_t)&sched_setaffinity_fake },
  { "sched_yield", (uintptr_t)&sched_yield },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "setjmp", (uintptr_t)&setjmp },
  { "setlocale", (uintptr_t)&setlocale },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sigemptyset", (uintptr_t)&sigemptyset_fake },
  { "sin", (uintptr_t)&sin },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "snprintf", (uintptr_t)&snprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "stat", (uintptr_t)&stat_fake },
  { "stat64", (uintptr_t)&stat_fake },
  { "stderr", (uintptr_t)&stderr_ptr },
  { "stdout", (uintptr_t)&stdout_ptr },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strcpy", (uintptr_t)&strcpy },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strftime", (uintptr_t)&strftime },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strnlen", (uintptr_t)&strnlen },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "swprintf", (uintptr_t)&swprintf },
  { "syscall", (uintptr_t)&syscall_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "syslog", (uintptr_t)&ret0 },
  { "tan", (uintptr_t)&tan },
  { "time", (uintptr_t)&time },
  { "tolower_l", (uintptr_t)&tolower_l_fake },
  { "toupper_l", (uintptr_t)&toupper_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "unlink", (uintptr_t)&unlink },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "usleep", (uintptr_t)&usleep },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },
  { "wctob", (uintptr_t)&wctob },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

// BSD ctype mask bits (newlib/bionic legacy layout)
#define _CTYPE_U 0x01  // upper
#define _CTYPE_L 0x02  // lower
#define _CTYPE_N 0x04  // digit
#define _CTYPE_S 0x08  // space
#define _CTYPE_P 0x10  // punct
#define _CTYPE_C 0x20  // control
#define _CTYPE_X 0x40  // hex digit
#define _CTYPE_B 0x80  // printable space (blank)

static EGLBoolean vk_egl_initialize(EGLDisplay display, EGLint *major,
                                    EGLint *minor) {
  (void)display;
  if (major) *major = 0;
  if (minor) *minor = 0;
  return EGL_FALSE;
}

static EGLBoolean vk_egl_choose_config(EGLDisplay display,
                                       const EGLint *attributes,
                                       EGLConfig *configs, EGLint config_size,
                                       EGLint *count) {
  (void)display;
  (void)attributes;
  (void)configs;
  (void)config_size;
  if (count) *count = 0;
  return EGL_FALSE;
}

static EGLBoolean vk_egl_get_config_attrib(EGLDisplay display,
                                           EGLConfig config,
                                           EGLint attribute, EGLint *value) {
  (void)display;
  (void)config;
  (void)attribute;
  if (value) *value = 0;
  return EGL_FALSE;
}

static EGLBoolean vk_egl_query_surface(EGLDisplay display, EGLSurface surface,
                                       EGLint attribute, EGLint *value) {
  (void)display;
  (void)surface;
  (void)attribute;
  if (value) *value = 0;
  return EGL_FALSE;
}

static const char *vk_egl_query_string(EGLDisplay display, EGLint name) {
  (void)display;
  (void)name;
  return "";
}

static EGLint vk_egl_get_error(void) { return EGL_SUCCESS; }

static void set_import_address(const char *name, uintptr_t address) {
  for (size_t index = 0; index < dynlib_numfunctions; index++) {
    if (!strcmp(dynlib_functions[index].symbol, name)) {
      dynlib_functions[index].func = address;
      return;
    }
  }
}

static void select_native_graphics_imports(void) {
  /* Native display backends never create an Android GLES/EGL context. Vulkan
   * keeps its existing upload capture; Deko3D reads the producer banks itself. */
  for (size_t index = 0; index < dynlib_numfunctions; index++) {
    const char *name = dynlib_functions[index].symbol;
    if ((name[0] == 'g' && name[1] == 'l' &&
         name[2] >= 'A' && name[2] <= 'Z') ||
        !strncmp(name, "egl", 3))
      dynlib_functions[index].func = (uintptr_t)&ret0;
  }

  if (drastic_renderer_is_vulkan()) {
    set_import_address("glBindTexture",
                       (uintptr_t)&drastic_vk_capture_gl_bind_texture);
    set_import_address("glTexSubImage2D",
                       (uintptr_t)&drastic_vk_capture_gl_tex_sub_image_2d);
  }
  set_import_address("eglInitialize", (uintptr_t)&vk_egl_initialize);
  set_import_address("eglChooseConfig", (uintptr_t)&vk_egl_choose_config);
  set_import_address("eglGetConfigAttrib",
                     (uintptr_t)&vk_egl_get_config_attrib);
  set_import_address("eglQuerySurface", (uintptr_t)&vk_egl_query_surface);
  set_import_address("eglQueryString", (uintptr_t)&vk_egl_query_string);
  set_import_address("eglGetError", (uintptr_t)&vk_egl_get_error);
}

void update_imports(void) {
  // fill the legacy _ctype_ table from the C locale: index [c+1] for char c,
  // [0] left as the EOF guard (0).
  for (int c = 0; c < 256; c++) {
    unsigned char m = 0;
    if (isupper(c))  m |= _CTYPE_U;
    if (islower(c))  m |= _CTYPE_L;
    if (isdigit(c))  m |= _CTYPE_N;
    if (isspace(c))  m |= _CTYPE_S;
    if (ispunct(c))  m |= _CTYPE_P;
    if (iscntrl(c))  m |= _CTYPE_C;
    if (isxdigit(c)) m |= _CTYPE_X;
    if (c == ' ')    m |= _CTYPE_B;
    g_ctype_table[c + 1] = m;
  }
  g_ctype_table[0] = 0;
  if (drastic_renderer_is_vulkan() || drastic_renderer_is_deko3d())
    select_native_graphics_imports();
}
