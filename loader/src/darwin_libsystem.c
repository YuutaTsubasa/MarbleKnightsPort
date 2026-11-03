// Darwin libSystem -> bionic translation layer for code inside UnityFramework.
//
// Only what libil2cpp, the IL2CPP generated code and FMOD reach is covered;
// anything else is bound to a trap stub by the loader. Functions whose ABI is
// identical on both sides (string.h, math.h, most of stdlib) are passed through.
//
// Rules of thumb for Darwin arm64 vs AAPCS64 (Android):
//  * variadic arguments are all on the stack (see the thunks in trap.S);
//  * va_list is a plain `char *` to those stack slots;
//  * errno values 1..34 match Linux, the higher ones differ;
//  * struct layouts differ for stat, dirent, sockaddr, sigaction, utsname, addrinfo;
//  * open/mmap/fcntl flag bits, sysconf/sysctl names and pthread enums differ.
#define _GNU_SOURCE
#include "darwin.h"

#include <android/log.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/select.h>
#include <utime.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkdarwin", __VA_ARGS__)

mk_image *g_darwin_image;
const char *g_darwin_exe_path = "/data/local/tmp/mk/reprovisioned";

// ---- va_list ---------------------------------------------------------------
// Build an AAPCS64 va_list whose register save areas are empty, so every
// va_arg is read from the Darwin stack area.
typedef struct { void *stack, *gr_top, *vr_top; int gr_offs, vr_offs; } aapcs_va;
#define DARWIN_VA(name, ptr) va_list name; do { aapcs_va *v_ = (aapcs_va *)&name; \
    v_->stack = (ptr); v_->gr_top = v_->vr_top = 0; v_->gr_offs = v_->vr_offs = 0; } while (0)

int shim_vsnprintf(char *buf, size_t n, const char *fmt, char *ap) { DARWIN_VA(va, ap); return vsnprintf(buf, n, fmt, va); }
int shim_vsprintf(char *buf, const char *fmt, char *ap) { DARWIN_VA(va, ap); return vsprintf(buf, fmt, va); }
int shim_vprintf(const char *fmt, char *ap) { DARWIN_VA(va, ap); return vprintf(fmt, va); }
int shim_vfprintf(FILE *f, const char *fmt, char *ap) { DARWIN_VA(va, ap); return vfprintf(f, fmt, va); }
int shim_vsscanf(const char *s, const char *fmt, char *ap) { DARWIN_VA(va, ap); return vsscanf(s, fmt, va); }
int shim_vasprintf(char **out, const char *fmt, char *ap) { DARWIN_VA(va, ap); return vasprintf(out, fmt, va); }
void shim_vsyslog(int pri, const char *fmt, char *ap) {
    DARWIN_VA(va, ap);
    __android_log_vprint(ANDROID_LOG_INFO, "mksyslog", fmt, va);
}
static int d___vsnprintf_chk(char *buf, size_t n, int flag, size_t slen, const char *fmt, char *ap) {
    (void)flag; (void)slen;
    return shim_vsnprintf(buf, n, fmt, ap);
}

// ---- errno -----------------------------------------------------------------
static int errno_to_darwin(int e) {
    if (e < 35) return e;
    switch (e) {
    case EAGAIN: return 35; case EINPROGRESS: return 36; case EALREADY: return 37; case ENOTSOCK: return 38;
    case EDESTADDRREQ: return 39; case EMSGSIZE: return 40; case EPROTOTYPE: return 41; case ENOPROTOOPT: return 42;
    case EPROTONOSUPPORT: return 43; case EOPNOTSUPP: return 45; case EAFNOSUPPORT: return 47;
    case EADDRINUSE: return 48; case EADDRNOTAVAIL: return 49; case ENETDOWN: return 50; case ENETUNREACH: return 51;
    case ECONNABORTED: return 53; case ECONNRESET: return 54; case ENOBUFS: return 55; case EISCONN: return 56;
    case ENOTCONN: return 57; case ETIMEDOUT: return 60; case ECONNREFUSED: return 61; case ELOOP: return 62;
    case ENAMETOOLONG: return 63; case EHOSTUNREACH: return 65; case ENOTEMPTY: return 66; case ENOSYS: return 78;
    case EOVERFLOW: return 84; case ECANCELED: return 89; case EILSEQ: return 92;
    default: return e;
    }
}
// Darwin code reads errno through __error(); translate the bionic value in place.
static int *d___error(void) { errno = errno_to_darwin(errno); return &errno; }
#define FAIL_ERRNO(r) do { if ((r) < 0) errno = errno_to_darwin(errno); } while (0)

// ---- files -------------------------------------------------------------------
static int oflags_from_darwin(int f) {
    int r = f & 3;
    if (f & 0x4) r |= O_NONBLOCK;
    if (f & 0x8) r |= O_APPEND;
    if (f & 0x200) r |= O_CREAT;
    if (f & 0x400) r |= O_TRUNC;
    if (f & 0x800) r |= O_EXCL;
    if (f & 0x100) r |= O_NOFOLLOW;
    if (f & 0x100000) r |= O_DIRECTORY;
    if (f & 0x1000000) r |= O_CLOEXEC;
    if (f & 0x20000) r |= O_NOCTTY;
    return r;
}
static int oflags_to_darwin(int f) {
    int r = f & 3;
    if (f & O_NONBLOCK) r |= 0x4;
    if (f & O_APPEND) r |= 0x8;
    return r;
}
int shim_open(const char *path, int flags, long mode) {
    int r = open(path, oflags_from_darwin(flags), (mode_t)mode);
    FAIL_ERRNO(r);
    return r;
}
int shim_fcntl(int fd, int cmd, long arg) {
    int r;
    switch (cmd) {
    case 1: case 2: r = fcntl(fd, cmd, arg); break;                 // F_GETFD / F_SETFD (FD_CLOEXEC = 1 both)
    case 3: r = fcntl(fd, F_GETFL); if (r >= 0) r = oflags_to_darwin(r); break;
    case 4: r = fcntl(fd, F_SETFL, oflags_from_darwin((int)arg)); break;
    case 48: case 51: return 0;                                     // F_NOCACHE, F_FULLFSYNC
    default: LOG("fcntl cmd %d not translated", cmd); errno = EINVAL; r = -1;
    }
    FAIL_ERRNO(r);
    return r;
}
int shim_ioctl(int fd, unsigned long req, long arg) {
    if (req == 0x8004667e) return ioctl(fd, FIONBIO, (void *)arg);  // FIONBIO
    if (req == 0x4004667f) return ioctl(fd, FIONREAD, (void *)arg); // FIONREAD
    LOG("ioctl %#lx not translated", req);
    errno = 25;  // ENOTTY
    return -1;
}

struct darwin_timespec { long tv_sec, tv_nsec; };
struct darwin_stat {
    int32_t st_dev; uint16_t st_mode, st_nlink; uint64_t st_ino; uint32_t st_uid, st_gid; int32_t st_rdev;
    struct darwin_timespec st_atim, st_mtim, st_ctim, st_birthtim;
    int64_t st_size, st_blocks; int32_t st_blksize; uint32_t st_flags, st_gen; int32_t st_lspare; int64_t st_qspare[2];
};
_Static_assert(sizeof(struct darwin_stat) == 144, "darwin stat");
static void stat_to_darwin(const struct stat *s, struct darwin_stat *d) {
    memset(d, 0, sizeof(*d));
    d->st_dev = (int32_t)s->st_dev; d->st_mode = (uint16_t)s->st_mode; d->st_nlink = (uint16_t)s->st_nlink;
    d->st_ino = s->st_ino; d->st_uid = s->st_uid; d->st_gid = s->st_gid; d->st_rdev = (int32_t)s->st_rdev;
    d->st_atim.tv_sec = s->st_atim.tv_sec; d->st_atim.tv_nsec = s->st_atim.tv_nsec;
    d->st_mtim.tv_sec = s->st_mtim.tv_sec; d->st_mtim.tv_nsec = s->st_mtim.tv_nsec;
    d->st_ctim.tv_sec = s->st_ctim.tv_sec; d->st_ctim.tv_nsec = s->st_ctim.tv_nsec;
    d->st_birthtim = d->st_ctim;
    d->st_size = s->st_size; d->st_blocks = s->st_blocks; d->st_blksize = (int32_t)s->st_blksize;
}
static int d_stat(const char *p, struct darwin_stat *d) { struct stat s; int r = stat(p, &s); if (!r) stat_to_darwin(&s, d); FAIL_ERRNO(r); return r; }
static int d_lstat(const char *p, struct darwin_stat *d) { struct stat s; int r = lstat(p, &s); if (!r) stat_to_darwin(&s, d); FAIL_ERRNO(r); return r; }
static int d_fstat(int fd, struct darwin_stat *d) { struct stat s; int r = fstat(fd, &s); if (!r) stat_to_darwin(&s, d); FAIL_ERRNO(r); return r; }

struct darwin_dirent { uint64_t d_ino, d_seekoff; uint16_t d_reclen, d_namlen; uint8_t d_type; char d_name[1024]; };
typedef struct { DIR *dir; struct darwin_dirent ent; } darwin_dir;
static void *d_opendir(const char *p) {
    DIR *d = opendir(p);
    if (!d) { errno = errno_to_darwin(errno); return NULL; }
    darwin_dir *w = calloc(1, sizeof(*w));
    w->dir = d;
    return w;
}
static struct darwin_dirent *d_readdir(darwin_dir *w) {
    struct dirent *e = readdir(w->dir);
    if (!e) return NULL;
    w->ent.d_ino = e->d_ino; w->ent.d_seekoff = e->d_off; w->ent.d_type = e->d_type;
    size_t n = strnlen(e->d_name, sizeof(w->ent.d_name) - 1);
    memcpy(w->ent.d_name, e->d_name, n);
    w->ent.d_name[n] = 0;
    w->ent.d_namlen = (uint16_t)n;
    w->ent.d_reclen = (uint16_t)(offsetof(struct darwin_dirent, d_name) + n + 1);
    return &w->ent;
}
static int d_closedir(darwin_dir *w) { int r = closedir(w->dir); free(w); return r; }

static ssize_t d_read(int fd, void *b, size_t n) { ssize_t r = read(fd, b, n); FAIL_ERRNO(r); return r; }
static ssize_t d_write(int fd, const void *b, size_t n) { ssize_t r = write(fd, b, n); FAIL_ERRNO(r); return r; }
static int d_mkdir(const char *p, mode_t m) { int r = mkdir(p, m); FAIL_ERRNO(r); return r; }

static char *d_realpath(const char *p, char *out) { return realpath(p, out); }

// ---- memory --------------------------------------------------------------------
static void *d_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    int f = flags & 3;  // SHARED / PRIVATE
    if (flags & 0x10) f |= MAP_FIXED;
    if (flags & 0x1000) f |= MAP_ANONYMOUS;
    if (flags & 0x40) f |= MAP_NORESERVE;
    // MAP_JIT (0x800) and VM_MAKE_TAG in fd (when anonymous) have no meaning here.
    if (flags & 0x1000) fd = -1;
    void *r = mmap(addr, len, prot, f, fd, off);
    if (r == MAP_FAILED) errno = errno_to_darwin(errno);
    return r;
}
static int d_madvise(void *a, size_t n, int adv) {
    // Darwin: MADV_DONTNEED 4, MADV_FREE 5; Linux: DONTNEED 4, FREE 8.
    return madvise(a, n, adv == 5 ? MADV_DONTNEED : adv);
}
static size_t d_malloc_size(const void *p) { return p ? malloc_usable_size(p) : 0; }
static void d_memset_pattern16(void *b, const void *pat, size_t len) {
    uint8_t *p = b;
    while (len >= 16) { memcpy(p, pat, 16); p += 16; len -= 16; }
    memcpy(p, pat, len);
}
static void *d___memcpy_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memcpy(d, s, n); }
static void *d___memmove_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memmove(d, s, n); }
static void *d___memset_chk(void *d, int c, size_t n, size_t dl) { (void)dl; return memset(d, c, n); }

// vm_allocate(task, vm_address_t *addr, vm_size_t size, int flags): flags&1 = VM_FLAGS_ANYWHERE
static int d_vm_allocate(uint32_t task, uint64_t *addr, uint64_t size, int flags) {
    (void)task;
    void *p = mmap((flags & 1) ? NULL : (void *)(uintptr_t)*addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | ((flags & 1) ? 0 : MAP_FIXED), -1, 0);
    if (p == MAP_FAILED) return 3;  // KERN_NO_SPACE
    *addr = (uint64_t)(uintptr_t)p;
    return 0;
}
static int d_vm_deallocate(uint32_t task, uint64_t addr, uint64_t size) { (void)task; munmap((void *)(uintptr_t)addr, size); return 0; }
// vm_read_overwrite(task, addr, size, data, *outsize): safe memory read used by crash reporters / GC probes.
static int d_vm_read_overwrite(uint32_t task, uint64_t addr, uint64_t size, uint64_t data, uint64_t *out) {
    (void)task;
    memcpy((void *)(uintptr_t)data, (void *)(uintptr_t)addr, size);
    if (out) *out = size;
    return 0;
}

// ---- time ------------------------------------------------------------------------
static uint64_t d_mach_absolute_time(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static int d_mach_timebase_info(uint32_t *info) { info[0] = 1; info[1] = 1; return 0; }

// ---- system info -----------------------------------------------------------------
static long d_sysconf(int name) {
    switch (name) {
    case 3: return sysconf(_SC_CLK_TCK);
    case 5: return sysconf(_SC_OPEN_MAX);
    case 29: return sysconf(_SC_PAGESIZE);
    case 57: return sysconf(_SC_NPROCESSORS_CONF);
    case 58: return sysconf(_SC_NPROCESSORS_ONLN);
    case 200: return sysconf(_SC_PHYS_PAGES);
    default: LOG("sysconf %d not translated", name); errno = EINVAL; return -1;
    }
}
static int put_sysctl(void *oldp, size_t *oldlenp, const void *v, size_t n) {
    if (oldp) {
        if (*oldlenp < n) { *oldlenp = n; errno = 12; return -1; }
        memcpy(oldp, v, n);
    }
    *oldlenp = n;
    return 0;
}
static int d_sysctl(int *mib, unsigned n, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (n >= 2 && mib[0] == 6) {  // CTL_HW
        int i32; int64_t i64;
        switch (mib[1]) {
        case 3: case 25: i32 = (int)sysconf(_SC_NPROCESSORS_ONLN); return put_sysctl(oldp, oldlenp, &i32, 4);
        case 7: i32 = (int)sysconf(_SC_PAGESIZE); return put_sysctl(oldp, oldlenp, &i32, 4);
        case 24: case 5: case 6:
            i64 = (int64_t)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
            return mib[1] == 24 ? put_sysctl(oldp, oldlenp, &i64, 8) : put_sysctl(oldp, oldlenp, &i64, oldlenp && *oldlenp == 4 ? 4 : 8);
        case 1: return put_sysctl(oldp, oldlenp, "iPad13,1", 9);
        case 2: return put_sysctl(oldp, oldlenp, "J307AP", 7);
        }
    } else if (n >= 2 && mib[0] == 1) {  // CTL_KERN
        switch (mib[1]) {
        case 1: return put_sysctl(oldp, oldlenp, "Darwin", 7);
        case 2: return put_sysctl(oldp, oldlenp, "20.0.0", 7);
        case 14: {  // KERN_PROC: kinfo_proc; zeroed means "not traced"
            static uint8_t zero[648];
            return put_sysctl(oldp, oldlenp, zero, sizeof(zero));
        }
        }
    }
    LOG("sysctl {%d,%d} not translated", n > 0 ? mib[0] : -1, n > 1 ? mib[1] : -1);
    errno = 2;
    return -1;
}
static int d_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    static const struct { const char *n; int a, b; } map[] = {
        { "hw.ncpu", 6, 3 }, { "hw.activecpu", 6, 25 }, { "hw.logicalcpu", 6, 3 }, { "hw.physicalcpu", 6, 3 },
        { "hw.memsize", 6, 24 }, { "hw.pagesize", 6, 7 }, { "hw.machine", 6, 1 }, { "hw.model", 6, 2 },
        { "kern.ostype", 1, 1 }, { "kern.osrelease", 1, 2 }, { 0, 0, 0 } };
    for (int i = 0; map[i].n; i++)
        if (!strcmp(map[i].n, name)) { int mib[2] = { map[i].a, map[i].b }; return d_sysctl(mib, 2, oldp, oldlenp, newp, newlen); }
    if (!strcmp(name, "hw.cpufrequency")) { int64_t f = 2000000000; return put_sysctl(oldp, oldlenp, &f, 8); }
    LOG("sysctlbyname %s not translated", name);
    errno = 2;
    return -1;
}
struct darwin_utsname { char sysname[256], nodename[256], release[256], version[256], machine[256]; };
static int d_uname(struct darwin_utsname *d) {
    struct utsname u;
    int r = uname(&u);
    memset(d, 0, sizeof(*d));
    strcpy(d->sysname, "Darwin");
    strncpy(d->nodename, u.nodename, 255);
    strcpy(d->release, "20.0.0");
    strncpy(d->version, u.version, 255);
    strcpy(d->machine, "iPad13,1");
    return r;
}
static int d_getrlimit(int res, struct rlimit *rl) {
    static const int map[] = { RLIMIT_CPU, RLIMIT_FSIZE, RLIMIT_DATA, RLIMIT_STACK, RLIMIT_CORE, RLIMIT_AS,
                               RLIMIT_MEMLOCK, RLIMIT_NPROC, RLIMIT_NOFILE };
    if (res < 0 || res > 8) { errno = EINVAL; return -1; }
    return getrlimit(map[res], rl);
}
static int d__NSGetExecutablePath(char *buf, uint32_t *size) {
    size_t n = strlen(g_darwin_exe_path) + 1;
    if (*size < n) { *size = (uint32_t)n; return -1; }
    memcpy(buf, g_darwin_exe_path, n);
    return 0;
}
extern char **environ;
static char ***d__NSGetEnviron(void) { return &environ; }
static const char *d_getprogname(void) { return "reprovisioned"; }

static int d_backtrace(void **b, int n) { (void)b; (void)n; return 0; }

// ---- locale / ctype ----------------------------------------------------------------
// Darwin's <ctype.h> is inline code reading _DefaultRuneLocale.__runetype[c].
struct darwin_rune_locale {
    char magic[8], encoding[32];
    void *sgetrune, *sputrune;
    int32_t invalid_rune;
    uint32_t runetype[256];
    int32_t maplower[256], mapupper[256];
    uint8_t rest[128];
};
struct darwin_rune_locale d_DefaultRuneLocale;
static void init_rune_locale(void) {
    memcpy(d_DefaultRuneLocale.magic, "RuneMagA", 8);
    strcpy(d_DefaultRuneLocale.encoding, "NONE");
    for (int c = 0; c < 256; c++) {
        uint32_t t = 0;
        if (c < 128) {
            if (isalpha(c)) t |= 0x100; if (iscntrl(c)) t |= 0x200; if (isdigit(c)) t |= 0x400 | (c - '0');
            if (isgraph(c)) t |= 0x800; if (islower(c)) t |= 0x1000; if (ispunct(c)) t |= 0x2000;
            if (isspace(c)) t |= 0x4000; if (isupper(c)) t |= 0x8000; if (isxdigit(c)) t |= 0x10000;
            if (c == ' ' || c == '\t') t |= 0x20000; if (isprint(c)) t |= 0x40000;
        }
        d_DefaultRuneLocale.runetype[c] = t;
        d_DefaultRuneLocale.maplower[c] = c < 128 ? tolower(c) : c;
        d_DefaultRuneLocale.mapupper[c] = c < 128 ? toupper(c) : c;
    }
}
static unsigned long d___maskrune(int c, unsigned long mask) {
    return (c >= 0 && c < 256) ? (d_DefaultRuneLocale.runetype[c] & mask) : 0;
}
static int d___tolower(int c) { return (c >= 0 && c < 128) ? tolower(c) : c; }
static int d___toupper(int c) { return (c >= 0 && c < 128) ? toupper(c) : c; }
static char *d_setlocale(int cat, const char *l) {
    static const int map[] = { LC_ALL, LC_COLLATE, LC_CTYPE, LC_MONETARY, LC_NUMERIC, LC_TIME, LC_MESSAGES };
    return setlocale(cat >= 0 && cat <= 6 ? map[cat] : LC_ALL, l);
}
static locale_t d_newlocale(int mask, const char *l, locale_t base) { (void)mask; (void)base; return newlocale(LC_ALL_MASK, l ? l : "C", 0); }

// ---- signals -------------------------------------------------------------------------
static int sig_from_darwin(int s) {
    switch (s) {
    case 7: return SIGSYS - 0;  // SIGEMT has no Linux equivalent; nobody should send it
    case 10: return SIGBUS; case 12: return SIGSYS; case 16: return SIGURG; case 17: return SIGSTOP;
    case 18: return SIGTSTP; case 19: return SIGCONT; case 20: return SIGCHLD; case 23: return SIGIO;
    case 30: return SIGUSR1; case 31: return SIGUSR2;
    default: return s;  // 1..6, 8, 9, 11, 13..15, 21, 22, 24..29 match
    }
}
static int d_raise(int s) { return raise(sig_from_darwin(s)); }
static void *d_signal(int s, void *h) {
    int ls = sig_from_darwin(s);
    if (h == SIG_IGN || h == SIG_DFL) return (void *)signal(ls, (sighandler_t)h);
    LOG("signal(%d) handler ignored", s);
    return SIG_DFL;
}
static int d_sigaction(int s, const void *act, void *old) {
    // Handlers written for Darwin expect Darwin siginfo/ucontext; we don't install them.
    // Unity's crash reporter and the GC don't need them for our purposes.
    (void)act;
    if (old) memset(old, 0, 16);
    LOG("sigaction(%d) ignored", s);
    return 0;
}
static int d_sigaltstack(const void *ss, void *old) { (void)ss; if (old) memset(old, 0, 24); return 0; }

// ---- pthread -------------------------------------------------------------------------
// Darwin objects are larger than bionic's, so the bionic object lives at the
// start of the Darwin storage. Static initializers carry a signature word.
#define DARWIN_MUTEX_SIG_INIT 0x32AAABA7L
#define DARWIN_ERRCHK_MUTEX_SIG_INIT 0x32AAABA1L
#define DARWIN_RECURSIVE_MUTEX_SIG_INIT 0x32AAABA2L
#define DARWIN_FIRSTFIT_MUTEX_SIG_INIT 0x32AAABA3L
#define DARWIN_COND_SIG_INIT 0x3CB0B1BBL
#define DARWIN_RWLOCK_SIG_INIT 0x2DA8B3B4L
static pthread_mutex_t g_static_init_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *mtx(void *m) {
    _Atomic long *sig = (_Atomic long *)m;
    long s = atomic_load_explicit(sig, memory_order_acquire);
    if ((s & ~0xFL) == 0x32AAABA0L) {
        pthread_mutex_lock(&g_static_init_lock);
        s = atomic_load(sig);
        if ((s & ~0xFL) == 0x32AAABA0L) {
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            if (s == DARWIN_RECURSIVE_MUTEX_SIG_INIT) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            if (s == DARWIN_ERRCHK_MUTEX_SIG_INIT) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
            pthread_mutex_t tmp;
            pthread_mutex_init(&tmp, &a);
            memcpy((char *)m + 8, (char *)&tmp + 8, sizeof(tmp) - 8);
            atomic_store(sig, *(long *)&tmp);
        }
        pthread_mutex_unlock(&g_static_init_lock);
    }
    return (pthread_mutex_t *)m;
}
static pthread_cond_t *cnd(void *c) {
    _Atomic long *sig = (_Atomic long *)c;
    if (atomic_load_explicit(sig, memory_order_acquire) == DARWIN_COND_SIG_INIT) {
        long expect = DARWIN_COND_SIG_INIT;
        atomic_compare_exchange_strong(sig, &expect, 0);  // zeroed bionic cond == PTHREAD_COND_INITIALIZER
    }
    return (pthread_cond_t *)c;
}
static pthread_rwlock_t *rwl(void *l) {
    _Atomic long *sig = (_Atomic long *)l;
    if (atomic_load_explicit(sig, memory_order_acquire) == DARWIN_RWLOCK_SIG_INIT) {
        long expect = DARWIN_RWLOCK_SIG_INIT;
        atomic_compare_exchange_strong(sig, &expect, 0);
    }
    return (pthread_rwlock_t *)l;
}
static int d_pthread_mutex_init(void *m, const pthread_mutexattr_t *a) { return pthread_mutex_init(m, a); }
static int d_pthread_mutex_lock(void *m) { return pthread_mutex_lock(mtx(m)); }
static int d_pthread_mutex_trylock(void *m) { int r = pthread_mutex_trylock(mtx(m)); return r == EBUSY ? 16 : r; }
static int d_pthread_mutex_unlock(void *m) { return pthread_mutex_unlock(mtx(m)); }
static int d_pthread_mutex_destroy(void *m) { return pthread_mutex_destroy(mtx(m)); }
static int d_pthread_mutexattr_settype(pthread_mutexattr_t *a, int t) {
    // Darwin: NORMAL 0, ERRORCHECK 1, RECURSIVE 2. bionic: NORMAL 0, RECURSIVE 1, ERRORCHECK 2.
    return pthread_mutexattr_settype(a, t == 2 ? PTHREAD_MUTEX_RECURSIVE : t == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
}
static int d_pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int p) { (void)a; (void)p; return 0; }
static int d_pthread_cond_init(void *c, const void *a) { (void)a; return pthread_cond_init(c, NULL); }
static int d_pthread_cond_wait(void *c, void *m) { return pthread_cond_wait(cnd(c), mtx(m)); }
static int d_pthread_cond_timedwait(void *c, void *m, const struct timespec *t) {
    int r = pthread_cond_timedwait(cnd(c), mtx(m), t);
    return r == ETIMEDOUT ? 60 : r;
}
static int d_pthread_cond_timedwait_relative_np(void *c, void *m, const struct timespec *rel) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    now.tv_sec += rel->tv_sec;
    now.tv_nsec += rel->tv_nsec;
    if (now.tv_nsec >= 1000000000) { now.tv_sec++; now.tv_nsec -= 1000000000; }
    return d_pthread_cond_timedwait(c, m, &now);
}
static int d_pthread_cond_signal(void *c) { return pthread_cond_signal(cnd(c)); }
static int d_pthread_cond_broadcast(void *c) { return pthread_cond_broadcast(cnd(c)); }
static int d_pthread_cond_destroy(void *c) { return pthread_cond_destroy(cnd(c)); }
static int d_pthread_rwlock_init(void *l, const void *a) { (void)a; return pthread_rwlock_init(l, NULL); }
static int d_pthread_rwlock_rdlock(void *l) { return pthread_rwlock_rdlock(rwl(l)); }
static int d_pthread_rwlock_wrlock(void *l) { return pthread_rwlock_wrlock(rwl(l)); }
static int d_pthread_rwlock_unlock(void *l) { return pthread_rwlock_unlock(rwl(l)); }
static int d_pthread_rwlock_destroy(void *l) { return pthread_rwlock_destroy(rwl(l)); }
static int d_pthread_attr_setdetachstate(pthread_attr_t *a, int s) {
    return pthread_attr_setdetachstate(a, s == 2 ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
}
static int d_pthread_attr_setschedpolicy(pthread_attr_t *a, int p) { (void)a; (void)p; return 0; }
static int d_pthread_attr_setschedparam(pthread_attr_t *a, const void *p) { (void)a; (void)p; return 0; }
static int d_pthread_setschedparam(pthread_t t, int p, const void *s) { (void)t; (void)p; (void)s; return 0; }
static int d_pthread_getschedparam(pthread_t t, int *p, int *s) { (void)t; *p = 1; s[0] = 31; s[1] = 0; return 0; }
static int d_sched_get_priority_max(int p) { (void)p; return 47; }
static int d_sched_get_priority_min(int p) { (void)p; return 15; }
static int d_pthread_key_create(unsigned long *k, void (*dtor)(void *)) {
    pthread_key_t bk;
    int r = pthread_key_create(&bk, dtor);
    if (!r) *k = (unsigned long)bk;
    return r;
}
static int d_pthread_key_delete(unsigned long k) { return pthread_key_delete((pthread_key_t)k); }
static void *d_pthread_getspecific(unsigned long k) { return pthread_getspecific((pthread_key_t)k); }
static int d_pthread_setspecific(unsigned long k, const void *v) { return pthread_setspecific((pthread_key_t)k, v); }
static int d_pthread_setname_np(const char *n) {
    char b[16];
    strncpy(b, n, 15);
    b[15] = 0;
    return pthread_setname_np(pthread_self(), b);
}
static int d_pthread_setcancelstate(int s, int *old) { (void)s; if (old) *old = 1; return 0; }
static void *d_pthread_get_stackaddr_np(pthread_t t) {
    pthread_attr_t a;
    void *addr = NULL;
    size_t size = 0;
    if (pthread_getattr_np(t, &a) == 0) {
        pthread_attr_getstack(&a, &addr, &size);
        pthread_attr_destroy(&a);
    }
    return (char *)addr + size;  // Darwin returns the high end
}

// ---- mach threads (for Boehm GC stop-the-world) ---------------------------------------
// mach_port_t thread names are small integers indexing this table.
#define MAX_THREADS 512
typedef struct {
    pid_t tid;
    pthread_t pthread;
    sem_t ack, resume;
    volatile int suspended;
    ucontext_t ctx;
} mthread;
static mthread g_threads[MAX_THREADS];
static _Atomic int g_nthreads;
static __thread int t_port;
static int g_suspend_sig;

static void suspend_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    mthread *t = &g_threads[t_port - 0x1000];
    int saved = errno;
    memcpy(&t->ctx, uc, sizeof(ucontext_t));
    sem_post(&t->ack);
    while (sem_wait(&t->resume) && errno == EINTR) {}
    errno = saved;
}
static uint32_t d_mach_thread_self(void) {
    if (!t_port) {
        int i = atomic_fetch_add(&g_nthreads, 1);
        if (i >= MAX_THREADS) abort();
        g_threads[i].tid = gettid();
        g_threads[i].pthread = pthread_self();
        sem_init(&g_threads[i].ack, 0, 0);
        sem_init(&g_threads[i].resume, 0, 0);
        t_port = 0x1000 + i;
    }
    return (uint32_t)t_port;
}
static uint32_t d_pthread_mach_thread_np(pthread_t t) {
    if (pthread_equal(t, pthread_self())) return d_mach_thread_self();
    for (int i = 0; i < g_nthreads; i++)
        if (pthread_equal(g_threads[i].pthread, t)) return 0x1000 + i;
    LOG("pthread_mach_thread_np: unknown thread");
    return 0;
}
static mthread *port_thread(uint32_t port) {
    int i = (int)port - 0x1000;
    return (i >= 0 && i < g_nthreads) ? &g_threads[i] : NULL;
}
static int d_thread_suspend(uint32_t port) {
    mthread *t = port_thread(port);
    if (!t) return 4;  // KERN_INVALID_ARGUMENT
    if (syscall(SYS_tgkill, getpid(), t->tid, g_suspend_sig)) return 5;
    while (sem_wait(&t->ack) && errno == EINTR) {}
    t->suspended = 1;
    return 0;
}
static int d_thread_resume(uint32_t port) {
    mthread *t = port_thread(port);
    if (!t || !t->suspended) return 4;
    t->suspended = 0;
    sem_post(&t->resume);
    return 0;
}
// thread_get_state(thread, ARM_THREAD_STATE64 = 6, state, &count): x0..x28, fp, lr, sp, pc, cpsr
static int d_thread_get_state(uint32_t port, int flavor, uint64_t *state, uint32_t *count) {
    mthread *t = port_thread(port);
    if (!t || flavor != 6 || *count < 68) return 4;
    const mcontext_t *m = &t->ctx.uc_mcontext;
    for (int i = 0; i < 29; i++) state[i] = m->regs[i];
    state[29] = m->regs[29];
    state[30] = m->regs[30];
    state[31] = m->sp;
    state[32] = m->pc;
    state[33] = (uint32_t)m->pstate;
    *count = 68;
    return 0;
}
// task_threads(task, thread_act_array_t *list, count): only threads that registered with us.
static int d_task_threads(uint32_t task, uint32_t **list, uint32_t *count) {
    (void)task;
    int n = g_nthreads;
    uint32_t *a = malloc(sizeof(uint32_t) * (n ? n : 1));
    for (int i = 0; i < n; i++) a[i] = 0x1000 + i;
    *list = a;
    *count = (uint32_t)n;
    return 0;
}
static int d_mach_port_deallocate(uint32_t task, uint32_t port) { (void)task; (void)port; return 0; }

typedef struct { void *(*fn)(void *); void *arg; } thread_start;
static void *thread_trampoline(void *p) {
    thread_start s = *(thread_start *)p;
    free(p);
    d_mach_thread_self();
    return s.fn(s.arg);
}
static int d_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    thread_start *s = malloc(sizeof(*s));
    s->fn = fn;
    s->arg = arg;
    return pthread_create(t, a, thread_trampoline, s);
}

// ---- mach semaphores ---------------------------------------------------------------------
#define MAX_SEMS 4096
static sem_t *g_sems[MAX_SEMS];
static int d_semaphore_create(uint32_t task, uint32_t *out, int policy, int value) {
    (void)task; (void)policy;
    for (int i = 1; i < MAX_SEMS; i++) {
        if (!g_sems[i]) {
            sem_t *s = malloc(sizeof(sem_t));
            sem_init(s, 0, value);
            g_sems[i] = s;
            *out = (uint32_t)i;
            return 0;
        }
    }
    return 6;  // KERN_RESOURCE_SHORTAGE
}
static int d_semaphore_destroy(uint32_t task, uint32_t s) {
    (void)task;
    if (s < MAX_SEMS && g_sems[s]) { sem_destroy(g_sems[s]); free(g_sems[s]); g_sems[s] = NULL; }
    return 0;
}
static int d_semaphore_signal(uint32_t s) { if (s >= MAX_SEMS || !g_sems[s]) return 4; sem_post(g_sems[s]); return 0; }
static int d_semaphore_wait(uint32_t s) {
    if (s >= MAX_SEMS || !g_sems[s]) return 4;
    while (sem_wait(g_sems[s]) && errno == EINTR) {}
    return 0;
}

// ---- dyld ------------------------------------------------------------------------------------
static uint32_t d__dyld_image_count(void) { return 1; }
static const void *d__dyld_get_image_header(uint32_t i) { return i == 0 ? g_darwin_image->base : NULL; }
static intptr_t d__dyld_get_image_vmaddr_slide(uint32_t i) { return i == 0 ? g_darwin_image->slide : 0; }
static const char *d__dyld_get_image_name(uint32_t i) {
    return i == 0 ? "/private/var/containers/Bundle/Application/reprovisioned.app/Frameworks/UnityFramework.framework/UnityFramework" : NULL;
}
static void d__dyld_register_func_for_add_image(void (*fn)(const void *, intptr_t)) {
    fn(g_darwin_image->base, g_darwin_image->slide);
}
static void d__dyld_register_func_for_remove_image(void (*fn)(const void *, intptr_t)) { (void)fn; }
// Returns the section_64 inside the loaded header; callers add the slide to ->addr themselves.
static const void *d_getsectbynamefromheader_64(const uint8_t *mh, const char *seg, const char *sect) {
    uint32_t ncmds = *(const uint32_t *)(mh + 16);
    const uint8_t *lc = mh + 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = *(const uint32_t *)lc, size = *(const uint32_t *)(lc + 4);
        if (cmd == 0x19 && !strncmp((const char *)lc + 8, seg, 16)) {
            uint32_t nsects = *(const uint32_t *)(lc + 64);
            const uint8_t *s = lc + 72;
            for (uint32_t k = 0; k < nsects; k++, s += 80)
                if (!strncmp((const char *)s, sect, 16)) return s;
        }
        lc += size;
    }
    return NULL;
}

// dlopen/dlsym: everything the game asks for is either inside UnityFramework
// (P/Invoke "__Internal") or provided by this shim.
static int g_fake_handle;
static void *d_dlopen(const char *path, int mode) {
    (void)mode;
    LOG("dlopen(%s)", path ? path : "(null)");
    return &g_fake_handle;
}
static void *d_dlsym(void *h, const char *name) {
    (void)h;
    char buf[512];
    snprintf(buf, sizeof(buf), "_%s", name);
    void *p = mk_export(g_darwin_image, buf);
    if (!p) p = darwin_resolve(name, "dlsym", 0);
    if (!p) LOG("dlsym(%s) not found", name);
    return p;
}
static int d_dlclose(void *h) { (void)h; return 0; }
static const char *d_dlerror(void) { return "symbol not found"; }
static int d_dladdr(const void *addr, void *info) { (void)addr; memset(info, 0, 32); return 0; }

// ---- dispatch (only stubs: nothing in the il2cpp path needs it to run) ----------------------------
static void d_dispatch_async(void *q, void *block) { (void)q; (void)block; LOG("dispatch_async ignored"); }
static void d_dispatch_once(long *pred, void *block) {
    // Block layout: isa, flags, reserved, invoke(block)
    if (*pred != ~0L) {
        *pred = ~0L;
        void (*invoke)(void *) = *(void (**)(void *))((char *)block + 16);
        invoke(block);
    }
}
// dispatch_time_t is nanoseconds on the monotonic clock here; DISPATCH_TIME_FOREVER = ~0.
static uint64_t d_dispatch_time(uint64_t when, int64_t delta) {
    if (when == ~0ull) return when;
    if (when == 0) when = d_mach_absolute_time();
    return when + delta;
}
static void *d_dispatch_semaphore_create(long value) {
    sem_t *s = malloc(sizeof(sem_t));
    sem_init(s, 0, (unsigned)value);
    return s;
}
static long d_dispatch_semaphore_signal(void *s) { sem_post(s); return 0; }
static long d_dispatch_semaphore_wait(void *s, uint64_t timeout) {
    if (timeout == ~0ull) {
        while (sem_wait(s) && errno == EINTR) {}
        return 0;
    }
    struct timespec ts;
    uint64_t now = d_mach_absolute_time();
    if (timeout <= now) return sem_trywait(s) ? 1 : 0;
    // sem_timedwait wants CLOCK_REALTIME
    uint64_t rel = timeout - now;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += rel / 1000000000ull;
    ts.tv_nsec += rel % 1000000000ull;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    while (sem_timedwait(s, &ts)) {
        if (errno == ETIMEDOUT) return 1;
    }
    return 0;
}
static void d_dispatch_release(void *o) { (void)o; }
static void d_dispatch_once_f(long *pred, void *ctx, void (*fn)(void *)) { if (*pred != ~0L) { *pred = ~0L; fn(ctx); } }

// ---- OSAtomic / OSSpinLock ----------------------------------------------------------------------
static int32_t d_OSAtomicAdd32Barrier(int32_t v, _Atomic int32_t *p) { return atomic_fetch_add(p, v) + v; }
static int64_t d_OSAtomicAdd64Barrier(int64_t v, _Atomic int64_t *p) { return atomic_fetch_add(p, v) + v; }
static int d_OSAtomicCompareAndSwap32Barrier(int32_t o, int32_t n, _Atomic int32_t *p) { return atomic_compare_exchange_strong(p, &o, n); }
static int d_OSAtomicCompareAndSwapPtrBarrier(void *o, void *n, void *_Atomic *p) { return atomic_compare_exchange_strong(p, &o, n); }
static void d_OSMemoryBarrier(void) { atomic_thread_fence(memory_order_seq_cst); }
static void d_OSSpinLockLock(_Atomic int32_t *l) { int32_t z; while (z = 0, !atomic_compare_exchange_weak(l, &z, 1)) sched_yield(); }
static void d_OSSpinLockUnlock(_Atomic int32_t *l) { atomic_store(l, 0); }

// ---- misc -----------------------------------------------------------------------------------------
static void d___assert_rtn(const char *fn, const char *file, int line, const char *expr) {
    LOG("assert %s at %s:%d (%s)", expr, file, line, fn);
    abort();
}
static void d___stack_chk_fail(void) { LOG("stack smashing detected"); abort(); }
typedef struct { double s, c; } sincos_d;
typedef struct { float s, c; } sincos_f;
static sincos_d d___sincos_stret(double x) { sincos_d r = { sin(x), cos(x) }; return r; }
static sincos_f d___sincosf_stret(float x) { sincos_f r = { sinf(x), cosf(x) }; return r; }
static double d___exp10(double x) { return pow(10.0, x); }
static float d___exp10f(float x) { return powf(10.0f, x); }
extern int __cxa_atexit(void (*)(void *), void *, void *);
static void d_bzero(void *p, size_t n) { memset(p, 0, n); }
// Darwin timeval is { long; int32 } + 4 bytes of padding the caller may leave uninitialized.
static int d_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
    struct timeval t, *pt = NULL;
    if (tv) { t.tv_sec = tv->tv_sec; t.tv_usec = (int32_t)tv->tv_usec; pt = &t; }
    int rc = select(n, r, w, e, pt);
    FAIL_ERRNO(rc);
    return rc;
}
static int d_gettimeofday(struct timeval *tv, void *tz) { return gettimeofday(tv, tz); }

// Darwin FILE* is opaque to the game; hand out bionic's directly.
FILE *d___stdinp, *d___stdoutp, *d___stderrp;
long d___stack_chk_guard = 0x5e1f6a1d2c3b4a59;
uint32_t d_mach_task_self_ = 0x103;
uint64_t d_vm_page_size, d_vm_page_mask;
uint32_t d_NDR_record[2] = { 0x00000100, 0 };
uint8_t d__FE_DFL_ENV[16];
void *d__NSConcreteGlobalBlock[32], *d__NSConcreteStackBlock[32];
uint8_t d__dispatch_main_q[128], d__dispatch_source_type_timer[64];

// ---- symbol table ---------------------------------------------------------------------------------
extern int darwin_open(), darwin_fcntl(), darwin_ioctl(), darwin_sprintf(), darwin_snprintf(), darwin_printf(),
    darwin_fprintf(), darwin_sscanf(), darwin_setjmp(), darwin_longjmp(), darwin_chkstk(), darwin_asprintf();

#define E(n) { #n, (void *)d_##n }
#define P(n) { #n, (void *)n }
#define A(n, f) { n, (void *)f }
static const darwin_sym g_libsystem[] = {
    // translated
    E(__error), E(stat), E(lstat), E(fstat), E(opendir), E(readdir), E(closedir), E(read), E(write), E(mkdir),
    E(mmap), E(madvise), E(malloc_size), E(memset_pattern16), E(__memcpy_chk), E(__memmove_chk), E(__memset_chk),
    E(vm_allocate), E(vm_deallocate), E(vm_read_overwrite), E(mach_absolute_time), E(mach_timebase_info),
    E(sysconf), E(sysctl), E(sysctlbyname), E(uname), E(getrlimit), E(_NSGetExecutablePath), E(_NSGetEnviron),
    E(getprogname), E(backtrace), E(__maskrune), E(__tolower), E(__toupper), E(setlocale), E(newlocale), E(raise),
    E(signal), E(sigaction), E(sigaltstack), E(pthread_mutex_init), E(pthread_mutex_lock), E(pthread_mutex_trylock),
    E(pthread_mutex_unlock), E(pthread_mutex_destroy), E(pthread_mutexattr_settype), E(pthread_mutexattr_setprotocol),
    E(pthread_cond_init), E(pthread_cond_wait), E(pthread_cond_timedwait), E(pthread_cond_timedwait_relative_np),
    E(pthread_cond_signal), E(pthread_cond_broadcast), E(pthread_cond_destroy), E(pthread_rwlock_init),
    E(pthread_rwlock_rdlock), E(pthread_rwlock_wrlock), E(pthread_rwlock_unlock), E(pthread_rwlock_destroy),
    E(pthread_attr_setdetachstate), E(pthread_attr_setschedpolicy), E(pthread_attr_setschedparam),
    E(pthread_setschedparam), E(pthread_getschedparam), E(sched_get_priority_max), E(sched_get_priority_min),
    E(pthread_key_create), E(pthread_key_delete), E(pthread_getspecific), E(pthread_setspecific),
    E(pthread_setname_np), E(pthread_setcancelstate), E(pthread_get_stackaddr_np), E(pthread_create),
    E(pthread_mach_thread_np), E(mach_thread_self), E(thread_suspend), E(thread_resume), E(thread_get_state),
    E(task_threads), E(mach_port_deallocate), E(semaphore_create), E(semaphore_destroy), E(semaphore_signal),
    E(semaphore_wait), E(_dyld_image_count), E(_dyld_get_image_header), E(_dyld_get_image_vmaddr_slide),
    E(_dyld_get_image_name), E(_dyld_register_func_for_add_image), E(_dyld_register_func_for_remove_image),
    E(getsectbynamefromheader_64), E(dlopen), E(dlsym), E(dlclose), E(dlerror), E(dladdr), E(dispatch_async),
    E(dispatch_once), E(dispatch_once_f), E(dispatch_time), E(dispatch_semaphore_create),
    E(dispatch_semaphore_signal), E(dispatch_semaphore_wait), E(dispatch_release), E(OSAtomicAdd32Barrier), E(OSAtomicAdd64Barrier),
    E(OSAtomicCompareAndSwap32Barrier), E(OSAtomicCompareAndSwapPtrBarrier), E(OSMemoryBarrier),
    E(OSSpinLockLock), E(OSSpinLockUnlock), E(__assert_rtn), E(__stack_chk_fail), E(__sincos_stret),
    E(__sincosf_stret), E(__exp10), E(__exp10f), E(gettimeofday), E(__vsnprintf_chk), E(realpath),
    A("realpath$DARWIN_EXTSN", d_realpath),
    A("open", darwin_open), A("fcntl", darwin_fcntl), A("ioctl", darwin_ioctl), A("sprintf", darwin_sprintf),
    A("snprintf", darwin_snprintf), A("printf", darwin_printf), A("fprintf", darwin_fprintf),
    A("sscanf", darwin_sscanf), A("asprintf", darwin_asprintf), A("vsnprintf", shim_vsnprintf),
    A("vfprintf", shim_vfprintf), A("_setjmp", darwin_setjmp), A("setjmp", darwin_setjmp),
    A("longjmp", darwin_longjmp), A("_longjmp", darwin_longjmp), A("__chkstk_darwin", darwin_chkstk),
    // data
    A("__stdinp", &d___stdinp), A("__stdoutp", &d___stdoutp), A("__stderrp", &d___stderrp),
    A("__stack_chk_guard", &d___stack_chk_guard), A("mach_task_self_", &d_mach_task_self_),
    A("vm_page_size", &d_vm_page_size), A("vm_page_mask", &d_vm_page_mask), A("NDR_record", d_NDR_record),
    A("_DefaultRuneLocale", &d_DefaultRuneLocale), A("_FE_DFL_ENV", d__FE_DFL_ENV),
    A("_NSConcreteGlobalBlock", d__NSConcreteGlobalBlock), A("_NSConcreteStackBlock", d__NSConcreteStackBlock),
    A("_dispatch_main_q", d__dispatch_main_q), A("_dispatch_source_type_timer", d__dispatch_source_type_timer),
    // identical ABI: pass through to bionic
    P(abort), P(exit), P(atexit), P(__cxa_atexit), P(malloc), P(calloc), P(realloc), P(free), P(posix_memalign),
    P(memchr), P(memcmp), P(memcpy), P(memmove), P(memset), E(bzero), P(strchr), P(strcmp), P(strcpy), P(strlen),
    P(strncmp), P(strncpy), P(strrchr), P(strstr), P(strlcpy), P(strnlen), P(strcat), P(strdup), P(strcasecmp),
    P(strerror), P(strtol), P(strtoul), P(strtoull), P(strtod), P(atoi), P(atol), P(qsort), P(bsearch), P(div),
    P(getenv), P(setenv), P(unsetenv), P(close), P(lseek), P(dup), P(dup2), P(pipe), P(access), P(unlink), P(rmdir),
    P(getcwd), P(isatty), P(ftruncate), P(truncate), P(rename), P(chmod), P(mkstemp), P(fopen), P(fclose),
    P(fflush), P(fseek), P(fseeko), P(ftell), P(ftello), P(fread), P(fwrite), P(fputc), P(fputs), P(puts),
    P(putchar), P(feof), P(ferror), P(clearerr), P(rewind), P(munmap), P(mprotect), P(getpagesize),
    P(nanosleep), P(usleep), P(sched_yield), P(time), P(clock), P(difftime), P(gmtime), P(localtime), P(mktime),
    P(strftime), P(getpid), P(getuid), P(gethostname), P(freelocale), E(select), P(poll), P(flock), P(utime),
    P(pthread_self), P(pthread_equal), P(pthread_join), P(pthread_detach), P(pthread_attr_init),
    P(pthread_attr_destroy), P(pthread_attr_setstacksize), P(pthread_mutexattr_init), P(pthread_mutexattr_destroy),
    P(pthread_atfork), P(acos), P(acosf), P(asin), P(asinf), P(atan), P(atan2), P(atan2f), P(atanf), P(cos), P(cosf),
    P(sin), P(sinf), P(tan), P(tanf), P(exp), P(exp2f), P(expf), P(fmod), P(fmodf), P(hypot), P(log), P(log10),
    P(log10f), P(log2f), P(logb), P(logf), P(modf), P(modff), P(pow), P(powf), P(scalbn), P(frexp), P(frexpf),
    P(ldexp), P(ldexpf), P(fegetenv), P(fesetenv),
    { 0, 0 } };

void *darwin_lookup(const darwin_sym *table, const char *name) {
    for (const darwin_sym *s = table; s->name; s++)
        if (!strcmp(s->name, name)) return s->addr;
    return NULL;
}

void *darwin_libsystem(const char *name) { return darwin_lookup(g_libsystem, name); }

void darwin_init(mk_image *img) {
    g_darwin_image = img;
    d___stdinp = stdin;
    d___stdoutp = stdout;
    d___stderrp = stderr;
    d_vm_page_size = (uint64_t)sysconf(_SC_PAGESIZE);
    d_vm_page_mask = d_vm_page_size - 1;
    init_rune_locale();
    g_suspend_sig = SIGRTMIN + 6;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = suspend_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(g_suspend_sig, &sa, NULL);
    d_mach_thread_self();
}
