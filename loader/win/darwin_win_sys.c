// More Darwin imports for the recompiled UnityFramework on Windows (see darwin_win.c for the
// conventions): threads and mach thread control (the GC suspends threads and reads their
// registers), semaphores, files, memory mapping, dyld/dlsym, sysctl, printf/scanf, time, math.
// Layouts follow the Android shims in loader/src/darwin_libsystem.c.
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <windows.h>

#include "win_rt.h"

#define SHIM(name) static void s_##name(cpu *c)
#define P(i) ((void *)(uintptr_t)X(i))
#define S(i) ((const char *)(uintptr_t)X(i))
#define I(i) ((int32_t)X(i))
#define U(i) ((uint32_t)X(i))
#define F(i) (c->v[i].f[0])
#define D(i) (c->v[i].d[0])
#define RETD(val) do { double r_ = (val); c->v[0].u[0] = 0; c->v[0].u[1] = 0; c->v[0].d[0] = r_; } while (0)
#define RETF(val) do { float r_ = (val); c->v[0].u[0] = 0; c->v[0].u[1] = 0; c->v[0].f[0] = r_; } while (0)
#define VA(k) ((va_list)(GSP))  // Darwin variadic arguments: 8-byte slots at the guest sp

void mk_thread_bind(mk_thread *t);
static char g_exe_path[MAX_PATH];

// ---- threads -------------------------------------------------------------------------------------
static DWORD WINAPI thread_start(LPVOID p) {
    mk_thread *t = p;
    mk_thread_bind(t);
    uint64_t x[1] = { t->start_arg };
    t->result = mk_guest_enter(t->start_fn, x, 1, NULL, 0, NULL);
    return 0;
}
SHIM(pthread_create) {  // (pthread_t *, attr, start, arg)
    mk_thread *t = mk_thread_new();
    t->start_fn = X(2);
    t->start_arg = X(3);
    *(mk_thread **)P(0) = t;
    HANDLE h = CreateThread(NULL, 1 << 20, thread_start, t, 0, NULL);
    if (!h) { RET(EAGAIN); return; }
    CloseHandle(h);  // the thread duplicates its own handle in mk_thread_bind
    RET(0);
}
SHIM(pthread_join) {
    mk_thread *t = P(0);
    while (!t->handle) Sleep(0);
    WaitForSingleObject(t->handle, INFINITE);
    if (X(1)) *(uint64_t *)P(1) = t->result;
    RET(0);
}
SHIM(pthread_detach) { ((mk_thread *)P(0))->detached = 1; RET(0); }
SHIM(pthread_get_stackaddr_np) { mk_thread *t = P(0); RET(t->gstack + t->gsize); }
SHIM(pthread_get_stacksize_np) { RET(((mk_thread *)P(0))->gsize); }
SHIM(pthread_mach_thread_np) { RET(((mk_thread *)P(0))->port); }
SHIM(pthread_threadid_np) { mk_thread *t = X(0) ? P(0) : mk_self(); *(uint64_t *)P(1) = t->tid; RET(0); }
SHIM(mach_thread_self) { RET(mk_self()->port); }
SHIM(thread_suspend) {
    mk_thread *t = mk_thread_by_port(U(0));
    if (!t || !t->handle) { RET(4); return; }
    if (SuspendThread(t->handle) == (DWORD)-1) { RET(5); return; }
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_CONTROL;
    GetThreadContext(t->handle, &ctx);  // waits until the thread is really stopped
    t->suspended = 1;
    RET(0);
}
SHIM(thread_resume) {
    mk_thread *t = mk_thread_by_port(U(0));
    if (!t || !t->suspended) { RET(4); return; }
    t->suspended = 0;
    ResumeThread(t->handle);
    RET(0);
}
// thread_get_state(port, ARM_THREAD_STATE64 = 6, state, &count): the guest registers of the
// thread's innermost guest frame (x0..x28, fp, lr, sp, pc, cpsr)
SHIM(thread_get_state) {
    mk_thread *t = mk_thread_by_port(U(0));
    uint64_t *st = P(2);
    uint32_t *count = P(3);
    if (!t || I(1) != 6 || *count < 68) { RET(4); return; }
    memset(st, 0, 68 * 4);
    cpu *g = t->cur;
    if (g) {
        for (int i = 0; i < 31; i++) st[i] = g->x[i];
        st[31] = g->x[31];
    } else st[31] = (uint64_t)(uintptr_t)(t->gstack + t->gsize);
    *count = 68;
    RET(0);
}
SHIM(task_threads) {
    int n = mk_thread_count();
    uint32_t *a = mk_malloc(sizeof(uint32_t) * (n ? n : 1));
    for (int i = 0; i < n; i++) a[i] = mk_thread_at(i)->port;
    *(uint32_t **)P(1) = a;
    *(uint32_t *)P(2) = (uint32_t)n;
    RET(0);
}
SHIM(mach_port_deallocate) { RET(0); }
SHIM(vm_deallocate) { RET(0); }  // task_threads' list: leaked, small

// ---- semaphores (mach and dispatch) ----------------------------------------------------------------
#define MAX_SEMS 4096
static HANDLE g_sems[MAX_SEMS];
SHIM(semaphore_create) {  // (task, semaphore_t *, policy, value)
    for (int i = 1; i < MAX_SEMS; i++)
        if (!g_sems[i]) { g_sems[i] = CreateSemaphoreA(NULL, I(3), 0x7fffffff, NULL); *(uint32_t *)P(1) = (uint32_t)i; RET(0); return; }
    RET(6);
}
SHIM(semaphore_destroy) { uint32_t s = U(1); if (s < MAX_SEMS && g_sems[s]) { CloseHandle(g_sems[s]); g_sems[s] = NULL; } RET(0); }
SHIM(semaphore_signal) { uint32_t s = U(0); if (s >= MAX_SEMS || !g_sems[s]) { RET(4); return; } ReleaseSemaphore(g_sems[s], 1, NULL); RET(0); }
SHIM(semaphore_wait) { uint32_t s = U(0); if (s >= MAX_SEMS || !g_sems[s]) { RET(4); return; } WaitForSingleObject(g_sems[s], INFINITE); RET(0); }
SHIM(semaphore_timedwait) {  // (sem, mach_timespec_t { uint32 sec; int32 nsec }) passed by value in x1
    uint32_t s = U(0);
    if (s >= MAX_SEMS || !g_sems[s]) { RET(4); return; }
    uint32_t sec = (uint32_t)X(1);
    int32_t nsec = (int32_t)(X(1) >> 32);
    DWORD ms = sec * 1000 + (DWORD)(nsec / 1000000);
    RET(WaitForSingleObject(g_sems[s], ms) == WAIT_OBJECT_0 ? 0 : 49);  // KERN_OPERATION_TIMED_OUT
}
static uint64_t now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
SHIM(dispatch_semaphore_create) { RET(CreateSemaphoreA(NULL, I(0), 0x7fffffff, NULL)); }
SHIM(dispatch_semaphore_signal) { ReleaseSemaphore(P(0), 1, NULL); RET(0); }
SHIM(dispatch_semaphore_wait) {
    uint64_t when = X(1);
    DWORD ms = INFINITE;
    if (when != ~0ull) { uint64_t now = now_ns(); ms = when > now ? (DWORD)((when - now) / 1000000) : 0; }
    RET(WaitForSingleObject(P(0), ms) == WAIT_OBJECT_0 ? 0 : 1);
}
SHIM(dispatch_time) { uint64_t when = X(0); if (when == ~0ull) { RET(when); return; } if (!when) when = now_ns(); RET(when + X(1)); }
SHIM(dispatch_release) { (void)c; }
SHIM(dispatch_retain) { (void)c; }
// dispatch_once(predicate, block): block layout isa, flags, reserved, invoke(block)
SHIM(dispatch_once) {
    volatile int64_t *pred = P(0);
    if (*pred == ~0ll) return;
    uint64_t invoke = *(uint64_t *)((uint8_t *)P(1) + 16);
    uint64_t x[1] = { X(1) };
    *pred = ~0ll;
    mk_guest_enter(invoke, x, 1, NULL, 0, NULL);
}
SHIM(dispatch_once_f) {
    volatile int64_t *pred = P(0);
    if (*pred == ~0ll) return;
    uint64_t x[1] = { X(1) };
    *pred = ~0ll;
    mk_guest_enter(X(2), x, 1, NULL, 0, NULL);
}
SHIM(dispatch_async) { mk_log("dispatch_async ignored"); }
SHIM(dispatch_get_global_queue) { static uint8_t q[64]; RET(q); }

// ---- atomics --------------------------------------------------------------------------------------
SHIM(OSAtomicAdd32Barrier) { RET((uint32_t)(InterlockedAdd((volatile LONG *)P(1), I(0)))); }
SHIM(OSAtomicAdd64Barrier) { RET(InterlockedAdd64((volatile LONG64 *)P(1), (LONG64)X(0))); }
SHIM(OSAtomicCompareAndSwap32Barrier) { RET(InterlockedCompareExchange((volatile LONG *)P(2), I(1), I(0)) == I(0)); }
SHIM(OSAtomicCompareAndSwapPtrBarrier) { RET(InterlockedCompareExchangePointer((void *volatile *)P(2), P(1), P(0)) == P(0)); }
SHIM(OSAtomicCompareAndSwap64Barrier) { RET(InterlockedCompareExchange64((volatile LONG64 *)P(2), (LONG64)X(1), (LONG64)X(0)) == (LONG64)X(0)); }
SHIM(OSMemoryBarrier) { (void)c; MemoryBarrier(); }
SHIM(OSSpinLockLock) { while (InterlockedCompareExchange((volatile LONG *)P(0), 1, 0)) SwitchToThread(); }
SHIM(OSSpinLockUnlock) { InterlockedExchange((volatile LONG *)P(0), 0); }
SHIM(OSSpinLockTry) { RET(InterlockedCompareExchange((volatile LONG *)P(0), 1, 0) == 0); }
SHIM(os_unfair_lock_lock) { while (InterlockedCompareExchange((volatile LONG *)P(0), 1, 0)) SwitchToThread(); }
SHIM(os_unfair_lock_unlock) { InterlockedExchange((volatile LONG *)P(0), 0); }

// ---- files ----------------------------------------------------------------------------------------
static int oflags(int f) {  // Darwin open flags -> MSVC
    int r = (f & 3) | _O_BINARY;
    if (f & 0x8) r |= _O_APPEND;
    if (f & 0x200) r |= _O_CREAT;
    if (f & 0x400) r |= _O_TRUNC;
    if (f & 0x800) r |= _O_EXCL;
    return r;
}
SHIM(open) { RET(_open(S(0), oflags(I(1)), _S_IREAD | _S_IWRITE)); }  // mode is variadic (stack): ignored
SHIM(close) { RET(_close(I(0))); }
SHIM(read) { RET((int64_t)_read(I(0), P(1), (unsigned)X(2))); }
SHIM(write) { RET((int64_t)_write(I(0), P(1), (unsigned)X(2))); }
SHIM(lseek) { RET(_lseeki64(I(0), (int64_t)X(1), I(2))); }
SHIM(pread) {
    int64_t at = _telli64(I(0));
    _lseeki64(I(0), (int64_t)X(3), 0);
    int r = _read(I(0), P(1), (unsigned)X(2));
    _lseeki64(I(0), at, 0);
    RET((int64_t)r);
}
SHIM(fsync) { RET(_commit(I(0))); }
SHIM(ftruncate) { RET(_chsize_s(I(0), (int64_t)X(1))); }
SHIM(fcntl) { RET(0); }
SHIM(flock) { RET(0); }
SHIM(isatty) { RET(_isatty(I(0))); }
SHIM(dup) { RET(_dup(I(0))); }
SHIM(dup2) { RET(_dup2(I(0), I(1))); }
SHIM(unlink) { RET(_unlink(S(0))); }
SHIM(rmdir) { RET(_rmdir(S(0))); }
SHIM(mkdir) { RET(_mkdir(S(0))); }
SHIM(rename) { _unlink(S(1)); RET(rename(S(0), S(1))); }
SHIM(access) { RET(_access(S(0), I(1) & 6)); }
SHIM(chmod) { RET(0); }
SHIM(getcwd) { RET(_getcwd(P(0), (int)X(1))); }
SHIM(chdir) { RET(_chdir(S(0))); }
SHIM(realpath) {
    char *out = X(1) ? P(1) : mk_malloc(1024);
    RET(_fullpath(out, S(0), 1024));
}
// Darwin struct stat (144 bytes)
typedef struct { int64_t sec, nsec; } dts;
typedef struct {
    int32_t st_dev; uint16_t st_mode, st_nlink; uint64_t st_ino; uint32_t st_uid, st_gid; int32_t st_rdev;
    dts st_atim, st_mtim, st_ctim, st_birthtim;
    int64_t st_size, st_blocks; int32_t st_blksize; uint32_t st_flags, st_gen; int32_t st_lspare; int64_t st_qspare[2];
} dstat;
static void to_dstat(const struct _stat64 *s, dstat *d) {
    memset(d, 0, sizeof(*d));
    d->st_dev = (int32_t)s->st_dev;
    d->st_mode = (uint16_t)((s->st_mode & _S_IFDIR) ? 0040000 | 0755 : 0100000 | 0644);
    d->st_nlink = 1;
    d->st_atim.sec = s->st_atime;
    d->st_mtim.sec = s->st_mtime;
    d->st_ctim.sec = s->st_ctime;
    d->st_birthtim.sec = s->st_ctime;
    d->st_size = s->st_size;
    d->st_blocks = (s->st_size + 511) / 512;
    d->st_blksize = 4096;
}
SHIM(stat) { struct _stat64 s; int r = _stat64(S(0), &s); if (!r) to_dstat(&s, P(1)); RET(r); }
SHIM(fstat) { struct _stat64 s; int r = _fstat64(I(0), &s); if (!r) to_dstat(&s, P(1)); RET(r); }
// Darwin dirent: { u64 ino, seekoff; u16 reclen, namlen; u8 type; char name[1024] }
typedef struct { HANDLE h; int first; WIN32_FIND_DATAA fd; uint8_t ent[8 + 8 + 2 + 2 + 1 + 1024 + 3]; } ddir;
SHIM(opendir) {
    char pat[MAX_PATH + 4];
    snprintf(pat, sizeof(pat), "%s\\*", S(0));
    ddir *d = calloc(1, sizeof(*d));
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); *_errno() = ENOENT; RET(0); return; }
    d->first = 1;
    RET(d);
}
SHIM(readdir) {
    ddir *d = P(0);
    if (!d->first && !FindNextFileA(d->h, &d->fd)) { RET(0); return; }
    d->first = 0;
    uint8_t *e = d->ent;
    memset(e, 0, sizeof(d->ent));
    size_t n = strlen(d->fd.cFileName);
    *(uint16_t *)(e + 16) = (uint16_t)(21 + n + 1);
    *(uint16_t *)(e + 18) = (uint16_t)n;
    e[20] = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;  // DT_DIR / DT_REG
    memcpy(e + 21, d->fd.cFileName, n + 1);
    RET(e);
}
SHIM(closedir) { ddir *d = P(0); FindClose(d->h); free(d); RET(0); }

// FILE*: the CRT's, opaque to the guest
SHIM(fopen) { char mode[16]; snprintf(mode, sizeof(mode), "%s%s", S(1), strchr(S(1), 'b') ? "" : "b"); RET(fopen(S(0), mode)); }
SHIM(fdopen) { RET(_fdopen(I(0), S(1))); }
SHIM(fileno) { RET(_fileno(P(0))); }
SHIM(fclose) { RET(fclose(P(0))); }
SHIM(fread) { RET(fread(P(0), X(1), X(2), P(3))); }
SHIM(fwrite) { RET(fwrite(P(0), X(1), X(2), P(3))); }
SHIM(fflush) { RET(fflush(P(0))); }
SHIM(fseek) { RET(_fseeki64(P(0), (int64_t)X(1), I(2))); }
SHIM(fseeko) { RET(_fseeki64(P(0), (int64_t)X(1), I(2))); }
SHIM(ftell) { RET(_ftelli64(P(0))); }
SHIM(ftello) { RET(_ftelli64(P(0))); }
SHIM(fgets) { RET(fgets(P(0), I(1), P(2))); }
SHIM(fputs) { RET(fputs(S(0), P(1))); }
SHIM(fputc) { RET(fputc(I(0), P(1))); }
SHIM(fgetc) { RET(fgetc(P(0))); }
SHIM(puts) { mk_log("guest: %s", S(0)); RET(0); }
SHIM(putchar) { RET(I(0)); }
SHIM(feof) { RET(feof(P(0))); }
SHIM(ferror) { RET(ferror(P(0))); }
SHIM(clearerr) { clearerr(P(0)); }
SHIM(rewind) { rewind(P(0)); }
SHIM(setvbuf) { RET(0); }

// ---- printf / scanf: Darwin long is 64-bit, so %l becomes %ll ------------------------------------
static const char *fix_fmt(const char *f, char *out, size_t n) {
    size_t k = 0;
    for (const char *p = f; *p && k + 4 < n; p++) {
        out[k++] = *p;
        if (*p != '%') continue;
        p++;
        while (*p && strchr("-+ #0123456789.*'", *p) && k + 4 < n) out[k++] = *p++;
        if (*p == 'l' && p[1] != 'l' && p[1] && strchr("diouxXn", p[1])) { out[k++] = 'l'; out[k++] = 'l'; continue; }
        if (*p == 'q') { out[k++] = 'l'; out[k++] = 'l'; continue; }
        if (*p == 't') { out[k++] = 'z'; continue; }
        if (!*p) break;
        out[k++] = *p;
    }
    out[k] = 0;
    return out;
}
#define FMT(i) char fbuf_[4096]; const char *fmt_ = fix_fmt(S(i), fbuf_, sizeof(fbuf_))
SHIM(vsnprintf) { FMT(2); RET(vsnprintf(P(0), X(1), fmt_, (va_list)P(3))); }
SHIM(vsprintf) { FMT(1); RET(vsprintf(P(0), fmt_, (va_list)P(2))); }
SHIM(vfprintf) { FMT(1); RET(vfprintf(P(0), fmt_, (va_list)P(2))); }
SHIM(vprintf) { FMT(0); char b[4096]; vsnprintf(b, sizeof(b), fmt_, (va_list)P(1)); mk_log("guest: %s", b); RET(strlen(b)); }
SHIM(vasprintf) {
    FMT(1);
    int n = _vscprintf(fmt_, (va_list)P(2));
    char *b = mk_malloc((size_t)n + 1);
    vsnprintf(b, (size_t)n + 1, fmt_, (va_list)P(2));
    *(char **)P(0) = b;
    RET(n);
}
SHIM(snprintf) { FMT(2); RET(vsnprintf(P(0), X(1), fmt_, VA(0))); }
SHIM(sprintf) { FMT(1); RET(vsprintf(P(0), fmt_, VA(0))); }
SHIM(fprintf) { FMT(1); RET(vfprintf(P(0), fmt_, VA(0))); }
SHIM(printf) { FMT(0); char b[4096]; vsnprintf(b, sizeof(b), fmt_, VA(0)); mk_log("guest: %s", b); RET(strlen(b)); }
SHIM(asprintf) {
    FMT(1);
    int n = _vscprintf(fmt_, VA(0));
    char *b = mk_malloc((size_t)n + 1);
    vsnprintf(b, (size_t)n + 1, fmt_, VA(0));
    *(char **)P(0) = b;
    RET(n);
}
SHIM(__vsnprintf_chk) { FMT(4); RET(vsnprintf(P(0), X(1), fmt_, (va_list)P(5))); }
SHIM(__snprintf_chk) { FMT(4); RET(vsnprintf(P(0), X(1), fmt_, VA(0))); }
SHIM(__sprintf_chk) { FMT(3); RET(vsprintf(P(0), fmt_, VA(0))); }
SHIM(sscanf) { FMT(1); RET(vsscanf(S(0), fmt_, VA(0))); }
SHIM(vsscanf) { FMT(1); RET(vsscanf(S(0), fmt_, (va_list)P(2))); }
SHIM(syslog) { FMT(1); char b[4096]; vsnprintf(b, sizeof(b), fmt_, VA(0)); mk_log("syslog: %s", b); }

// ---- memory mapping -------------------------------------------------------------------------------
SHIM(mmap) {  // (addr, len, prot, flags, fd, off)
    size_t len = X(1);
    int flags = I(3), prot = I(2);
    DWORD wprot = (prot & 2) ? PAGE_READWRITE : (prot & 1) ? PAGE_READONLY : PAGE_NOACCESS;
    if (flags & 0x1000) {  // MAP_ANON
        void *p = VirtualAlloc((flags & 0x10) ? P(0) : NULL, len, MEM_RESERVE | MEM_COMMIT, wprot == PAGE_NOACCESS ? PAGE_READWRITE : wprot);
        RET(p ? (uint64_t)(uintptr_t)p : ~0ull);
        return;
    }
    HANDLE fh = (HANDLE)_get_osfhandle(I(4));
    HANDLE m = CreateFileMappingA(fh, NULL, (prot & 2) ? PAGE_WRITECOPY : PAGE_READONLY, 0, 0, NULL);
    if (!m) { mk_log("mmap fd %d: CreateFileMapping failed (%lu)", I(4), GetLastError()); RET(~0ull); return; }
    uint64_t off = X(5);
    LARGE_INTEGER fsz;
    // POSIX maps whole pages past the end of the file; Windows views must stay inside it
    if (GetFileSizeEx(fh, &fsz) && off + len > (uint64_t)fsz.QuadPart) len = (size_t)((uint64_t)fsz.QuadPart - off);
    void *p = MapViewOfFile(m, (prot & 2) ? FILE_MAP_COPY : FILE_MAP_READ, (DWORD)(off >> 32), (DWORD)off, len);
    if (!p) mk_log("mmap fd %d len %zu off %llu: MapViewOfFile failed (%lu)", I(4), len, (unsigned long long)off, GetLastError());
    CloseHandle(m);
    RET(p ? (uint64_t)(uintptr_t)p : ~0ull);
}
SHIM(munmap) {
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(P(0), &mi, sizeof(mi)) && mi.Type == MEM_MAPPED) UnmapViewOfFile(P(0));
    else VirtualFree(P(0), X(1), MEM_DECOMMIT);
    RET(0);
}
SHIM(mprotect) {
    int prot = I(2);
    DWORD old, w = (prot & 2) ? PAGE_READWRITE : (prot & 1) ? PAGE_READONLY : PAGE_NOACCESS;
    VirtualAlloc(P(0), X(1), MEM_COMMIT, PAGE_READWRITE);
    RET(VirtualProtect(P(0), X(1), w, &old) ? 0 : -1);
}
SHIM(madvise) { RET(0); }
SHIM(vm_allocate) {  // (task, vm_address_t *addr, size, flags)
    void *p = VirtualAlloc(NULL, X(2), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    *(uint64_t *)P(1) = (uint64_t)(uintptr_t)p;
    RET(p ? 0 : 3);
}
SHIM(vm_read_overwrite) { memcpy(P(3), P(1), X(2)); *(uint64_t *)P(4) = X(2); RET(0); }

// ---- dyld, dlsym ----------------------------------------------------------------------------------
SHIM(_dyld_image_count) { RET(1); }
SHIM(_dyld_get_image_header) { RET(I(0) == 0 ? IMG : 0); }
SHIM(_dyld_get_image_vmaddr_slide) { RET(I(0) == 0 ? IMG : 0); }
SHIM(_dyld_get_image_name) { RET(I(0) == 0 ? (uint64_t)(uintptr_t)"/var/containers/Bundle/Application/MarbleKnights.app/Frameworks/UnityFramework.framework/UnityFramework" : 0); }
SHIM(_dyld_register_func_for_add_image) { uint64_t x[2] = { IMG, IMG }; mk_guest_enter(X(0), x, 2, NULL, 0, NULL); }
SHIM(_dyld_register_func_for_remove_image) { (void)c; }
SHIM(getsectbynamefromheader_64) {
    const uint8_t *mh = P(0);
    uint32_t ncmds = *(const uint32_t *)(mh + 16);
    const uint8_t *lc = mh + 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = *(const uint32_t *)lc, size = *(const uint32_t *)(lc + 4);
        if (cmd == 0x19 && !strncmp((const char *)lc + 8, S(1), 16)) {
            uint32_t nsects = *(const uint32_t *)(lc + 64);
            const uint8_t *s = lc + 72;
            for (uint32_t k = 0; k < nsects; k++, s += 80)
                if (!strncmp((const char *)s, S(2), 16)) { RET(s); return; }
        }
        lc += size;
    }
    RET(0);
}
static int g_fake_handle;
SHIM(dlopen) { mk_log("dlopen(%s)", X(0) ? S(0) : "(null)"); RET(&g_fake_handle); }
SHIM(dlsym) {
    char buf[512];
    snprintf(buf, sizeof(buf), "_%s", S(1));
    uint64_t a = mk_export(buf);
    if (!a) mk_log("dlsym(%s) not found", S(1));
    RET(a);
}
SHIM(dlclose) { RET(0); }
SHIM(dlerror) { RET("symbol not found"); }
SHIM(dladdr) { memset(P(1), 0, 32); RET(0); }

// ---- system information ---------------------------------------------------------------------------
static int sysctl_out(void *oldp, uint64_t *oldlenp, const void *v, size_t n) {
    if (oldlenp) { if (oldp && *oldlenp >= n) memcpy(oldp, v, n); *oldlenp = n; }
    return 0;
}
static int sysctl_name(const char *name, void *oldp, uint64_t *oldlenp) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    int32_t ncpu = (int32_t)si.dwNumberOfProcessors;
    int64_t mem = (int64_t)ms.ullTotalPhys;
    if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.activecpu") || !strcmp(name, "hw.logicalcpu") ||
        !strcmp(name, "hw.physicalcpu")) return sysctl_out(oldp, oldlenp, &ncpu, 4);
    if (!strcmp(name, "hw.memsize")) return sysctl_out(oldp, oldlenp, &mem, 8);
    if (!strcmp(name, "hw.machine")) return sysctl_out(oldp, oldlenp, "iPad8,1", 8);
    if (!strcmp(name, "hw.model")) return sysctl_out(oldp, oldlenp, "J317AP", 7);
    if (!strcmp(name, "kern.osversion")) return sysctl_out(oldp, oldlenp, "18A373", 7);
    if (!strcmp(name, "hw.cpufrequency")) { int64_t f = 2400000000ll; return sysctl_out(oldp, oldlenp, &f, 8); }
    mk_log("sysctl %s not provided", name);
    *_errno() = ENOENT;
    return -1;
}
SHIM(sysctlbyname) { RET(sysctl_name(S(0), P(1), P(2))); }
SHIM(sysctl) {  // (int *mib, u_int n, oldp, size_t *oldlenp, newp, newlen)
    const int32_t *mib = P(0);
    const char *name = NULL;
    if (mib[0] == 6 && mib[1] == 3) name = "hw.ncpu";
    else if (mib[0] == 6 && mib[1] == 24) name = "hw.memsize";
    else if (mib[0] == 6 && mib[1] == 1) name = "hw.machine";
    else if (mib[0] == 6 && mib[1] == 2) name = "hw.model";
    else if (mib[0] == 6 && mib[1] == 25) name = "hw.activecpu";
    if (name) { RET(sysctl_name(name, P(2), P(3))); return; }
    mk_log("sysctl mib %d.%d not provided", mib[0], mib[1]);
    RET(-1);
}
SHIM(uname) {  // 5 x 256 chars
    char *u = P(0);
    memset(u, 0, 5 * 256);
    strcpy(u, "Darwin");
    strcpy(u + 256, "MarbleKnights");
    strcpy(u + 512, "20.0.0");
    strcpy(u + 768, "Darwin Kernel Version 20.0.0");
    strcpy(u + 1024, "iPad8,1");
    RET(0);
}
SHIM(getrlimit) { uint64_t *rl = P(1); rl[0] = 8 << 20; rl[1] = ~0ull; RET(0); }
SHIM(setrlimit) { RET(0); }
SHIM(_NSGetExecutablePath) {
    size_t n = strlen(g_exe_path) + 1;
    uint32_t *sz = P(1);
    if (*sz < n) { *sz = (uint32_t)n; RET(-1); return; }
    memcpy(P(0), g_exe_path, n);
    RET(0);
}
static char *g_environ[] = { NULL };
static char **g_environ_p = g_environ;
SHIM(_NSGetEnviron) { RET(&g_environ_p); }
SHIM(getprogname) { RET("MarbleKnights"); }
SHIM(getpid) { RET(GetCurrentProcessId()); }
SHIM(getuid) { RET(501); }
SHIM(geteuid) { RET(501); }
SHIM(gethostname) { strncpy(P(0), "MarbleKnights", X(1)); RET(0); }
SHIM(backtrace) { RET(0); }
SHIM(signal) { RET(0); }
SHIM(sigaction) { if (X(2)) memset(P(2), 0, 16); RET(0); }
SHIM(sigaltstack) { RET(0); }
SHIM(sigemptyset) { *(uint32_t *)P(0) = 0; RET(0); }
SHIM(sigaddset) { RET(0); }
SHIM(sigfillset) { *(uint32_t *)P(0) = ~0u; RET(0); }
SHIM(pthread_sigmask) { RET(0); }
SHIM(raise) { mk_fatal("guest raise(%d)", I(0)); }
SHIM(pthread_atfork) { RET(0); }
SHIM(pthread_setcancelstate) { RET(0); }
SHIM(pthread_setschedparam) { RET(0); }
SHIM(pthread_getschedparam) { *(int32_t *)P(1) = 0; memset(P(2), 0, 8); RET(0); }
SHIM(sched_get_priority_max) { RET(47); }
SHIM(sched_get_priority_min) { RET(15); }
SHIM(_setjmp) { RET(0); }  // the GC only uses it to spill registers
SHIM(setjmp) { RET(0); }
SHIM(longjmp) { mk_fatal("guest longjmp: not supported"); }
SHIM(_longjmp) { mk_fatal("guest _longjmp: not supported"); }
SHIM(__chkstk_darwin) { (void)c; }

// ---- time -----------------------------------------------------------------------------------------
SHIM(gettimeofday) {  // Darwin timeval { long sec; int usec }
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10 - 11644473600000000ull;
    if (X(0)) { int64_t *tv = P(0); tv[0] = (int64_t)(t / 1000000); ((int32_t *)tv)[2] = (int32_t)(t % 1000000); }
    RET(0);
}
SHIM(clock_gettime) {  // Darwin clockid: REALTIME 0, MONOTONIC 6, MONOTONIC_RAW 4, *_CPUTIME 12/16
    int64_t *ts = P(1);
    if (I(0) == 0) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) * 100 - 11644473600000000000ull;
        ts[0] = (int64_t)(t / 1000000000ull);
        ts[1] = (int64_t)(t % 1000000000ull);
    } else {
        uint64_t t = now_ns();
        ts[0] = (int64_t)(t / 1000000000ull);
        ts[1] = (int64_t)(t % 1000000000ull);
    }
    RET(0);
}
SHIM(nanosleep) { const int64_t *ts = P(0); Sleep((DWORD)(ts[0] * 1000 + ts[1] / 1000000)); RET(0); }
// Darwin struct tm = 9 ints + long tm_gmtoff + char *tm_zone
static void to_dtm(const struct tm *t, uint8_t *d) {
    memset(d, 0, 56);
    memcpy(d, t, 9 * sizeof(int));
    static char zone[8] = "UTC";
    *(int64_t *)(d + 40) = 0;
    *(char **)(d + 48) = zone;
}
SHIM(localtime_r) { struct tm t; __time64_t v = *(int64_t *)P(0); _localtime64_s(&t, &v); to_dtm(&t, P(1)); RET(P(1)); }
SHIM(gmtime_r) { struct tm t; __time64_t v = *(int64_t *)P(0); _gmtime64_s(&t, &v); to_dtm(&t, P(1)); RET(P(1)); }
SHIM(localtime) { static uint8_t b[56]; struct tm t; __time64_t v = *(int64_t *)P(0); _localtime64_s(&t, &v); to_dtm(&t, b); RET(b); }
SHIM(gmtime) { static uint8_t b[56]; struct tm t; __time64_t v = *(int64_t *)P(0); _gmtime64_s(&t, &v); to_dtm(&t, b); RET(b); }
SHIM(mktime) { struct tm t; memcpy(&t, P(0), 9 * sizeof(int)); RET(_mktime64(&t)); }
SHIM(timegm) { struct tm t; memcpy(&t, P(0), 9 * sizeof(int)); RET(_mkgmtime64(&t)); }
SHIM(strftime) { struct tm t; memcpy(&t, P(3), 9 * sizeof(int)); RET(strftime(P(0), X(1), S(2), &t)); }
SHIM(tzset) { (void)c; }
SHIM(clock) { RET(clock()); }

// ---- locale / ctype ------------------------------------------------------------------------------
SHIM(setlocale) { RET("C"); }
SHIM(newlocale) { static int l; RET(&l); }
SHIM(uselocale) { static int l; RET(&l); }
SHIM(freelocale) { (void)c; }
SHIM(localeconv) { static char dot[] = ".", empty[] = ""; static char *lc[20]; lc[0] = dot; for (int i = 1; i < 20; i++) lc[i] = empty; RET(lc); }
SHIM(__maskrune) { extern uint8_t mk_rune_locale[]; uint32_t ch = U(0); RET(ch < 256 ? ((uint32_t *)(mk_rune_locale + 52))[ch] & U(1) : 0); }
SHIM(__tolower) { RET(U(0) < 128 ? tolower(I(0)) : I(0)); }
SHIM(__toupper) { RET(U(0) < 128 ? toupper(I(0)) : I(0)); }
SHIM(tolower) { RET(tolower(I(0))); }
SHIM(toupper) { RET(toupper(I(0))); }

// ---- more strings and stdlib ---------------------------------------------------------------------
SHIM(strlcat) {
    size_t dl = strlen(P(0)), sl = strlen(S(1)), n = X(2);
    if (dl < n) { size_t k = sl < n - dl - 1 ? sl : n - dl - 1; memcpy((char *)P(0) + dl, S(1), k); ((char *)P(0))[dl + k] = 0; }
    RET(dl + sl);
}
SHIM(strncat) { RET(strncat(P(0), S(1), X(2))); }
SHIM(strspn) { RET(strspn(S(0), S(1))); }
SHIM(strcspn) { RET(strcspn(S(0), S(1))); }
SHIM(strpbrk) { RET(strpbrk(S(0), S(1))); }
SHIM(strtok_r) { RET(strtok_s(P(0), S(1), P(2))); }
SHIM(strsep) {
    char **sp = P(0), *s = *sp;
    if (!s) { RET(0); return; }
    char *e = s + strcspn(s, S(1));
    if (*e) { *e = 0; *sp = e + 1; } else *sp = NULL;
    RET(s);
}
SHIM(strndup) { size_t n = strnlen(S(0), X(1)); char *d = mk_malloc(n + 1); memcpy(d, S(0), n); d[n] = 0; RET(d); }
SHIM(atol) { RET(_atoi64(S(0))); }
SHIM(atof) { RETD(atof(S(0))); }
SHIM(abs) { RET(abs(I(0))); }
SHIM(labs) { RET(llabs((int64_t)X(0))); }
SHIM(rand) { RET(rand()); }
SHIM(srand) { srand(U(0)); }
SHIM(random) { RET(rand()); }
SHIM(srandom) { srand(U(0)); }
SHIM(arc4random) { unsigned v; rand_s(&v); RET(v); }
SHIM(arc4random_uniform) { unsigned v; rand_s(&v); RET(X(0) ? v % U(0) : 0); }
SHIM(setenv) { RET(_putenv_s(S(0), S(1))); }
SHIM(unsetenv) { RET(_putenv_s(S(0), "")); }
// qsort with a guest comparator
static uint64_t g_qsort_cmp;
SHIM(qsort) {
    uint8_t *base = P(0);
    size_t n = X(1), sz = X(2);
    uint64_t cmp = X(3);
    uint8_t *tmp = malloc(sz);
    for (size_t i = 1; i < n; i++) {  // insertion sort: stable, fine for the small arrays this sees
        size_t j = i;
        memcpy(tmp, base + i * sz, sz);
        while (j > 0) {
            uint64_t x[2] = { (uint64_t)(uintptr_t)(base + (j - 1) * sz), (uint64_t)(uintptr_t)tmp };
            if ((int32_t)mk_guest_enter(cmp, x, 2, NULL, 0, NULL) <= 0) break;
            memcpy(base + j * sz, base + (j - 1) * sz, sz);
            j--;
        }
        memcpy(base + j * sz, tmp, sz);
    }
    free(tmp);
    (void)g_qsort_cmp;
}
SHIM(bsearch) {
    const uint8_t *base = P(1);
    size_t lo = 0, hi = X(2), sz = X(3);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint64_t x[2] = { X(0), (uint64_t)(uintptr_t)(base + mid * sz) };
        int32_t r = (int32_t)mk_guest_enter(X(4), x, 2, NULL, 0, NULL);
        if (!r) { RET(base + mid * sz); return; }
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    RET(0);
}

// ---- math -----------------------------------------------------------------------------------------
#define M1D(n) SHIM(n) { RETD(n(D(0))); }
#define M1F(n) SHIM(n) { RETF(n(F(0))); }
#define M2D(n) SHIM(n) { RETD(n(D(0), D(1))); }
#define M2F(n) SHIM(n) { RETF(n(F(0), F(1))); }
M1D(acos) M1F(acosf) M1D(asin) M1F(asinf) M1D(atan) M1F(atanf) M2D(atan2) M2F(atan2f) M1D(cos) M1F(cosf)
M1D(sin) M1F(sinf) M1D(tan) M1F(tanf) M1D(exp) M1F(expf) M1F(exp2f) M1D(exp2) M2D(fmod) M2F(fmodf) M2D(hypot)
M1D(log) M1F(logf) M1D(log10) M1F(log10f) M1F(log2f) M1D(log2) M1D(logb) M2D(pow) M2F(powf) M1D(cosh) M1D(sinh)
M1D(tanh) M1F(coshf) M1F(sinhf) M1F(tanhf) M1D(log1p) M1D(expm1) M1D(cbrt) M1D(round) M1F(roundf) M1D(trunc)
M1F(truncf) M1D(floor) M1F(floorf) M1D(ceil) M1F(ceilf) M1D(sqrt) M1F(sqrtf) M1D(rint) M1F(rintf)
M1D(nearbyint) M2D(remainder) M2D(copysign) M2F(copysignf) M1D(asinh) M1D(acosh) M1D(atanh)
SHIM(scalbn) { RETD(scalbn(D(0), I(0))); }
SHIM(ldexp) { RETD(ldexp(D(0), I(0))); }
SHIM(ldexpf) { RETF(ldexpf(F(0), I(0))); }
SHIM(frexp) { RETD(frexp(D(0), P(0))); }
SHIM(frexpf) { RETF(frexpf(F(0), P(0))); }
SHIM(modf) { RETD(modf(D(0), P(0))); }
SHIM(modff) { RETF(modff(F(0), P(0))); }
SHIM(lround) { RET(llround(D(0))); }
SHIM(lroundf) { RET(llroundf(F(0))); }
SHIM(__exp10) { RETD(pow(10.0, D(0))); }
SHIM(__exp10f) { RETF(powf(10.0f, F(0))); }
SHIM(__sincos_stret) { double s = sin(D(0)), co = cos(D(0)); c->v[0].u[1] = c->v[1].u[1] = 0; c->v[0].d[0] = s; c->v[1].d[0] = co; }
SHIM(__sincosf_stret) { float s = sinf(F(0)), co = cosf(F(0)); c->v[0].u[0] = c->v[0].u[1] = c->v[1].u[0] = c->v[1].u[1] = 0; c->v[0].f[0] = s; c->v[1].f[0] = co; }
SHIM(fegetenv) { memset(P(0), 0, 16); RET(0); }
SHIM(fesetenv) { RET(0); }
SHIM(fegetround) { RET(0); }

// ---- table --------------------------------------------------------------------------------------
typedef struct { const char *name; mk_import_fn fn; } sys_entry;
#define F_(n) { "_" #n, s_##n }
static const sys_entry g_sys[] = {
    F_(pthread_create), F_(pthread_join), F_(pthread_detach), F_(pthread_get_stackaddr_np), F_(pthread_get_stacksize_np),
    F_(pthread_mach_thread_np), F_(pthread_threadid_np), F_(mach_thread_self), F_(thread_suspend), F_(thread_resume),
    F_(thread_get_state), F_(task_threads), F_(mach_port_deallocate), F_(vm_deallocate), F_(semaphore_create),
    F_(semaphore_destroy), F_(semaphore_signal), F_(semaphore_wait), F_(semaphore_timedwait),
    F_(dispatch_semaphore_create), F_(dispatch_semaphore_signal), F_(dispatch_semaphore_wait), F_(dispatch_time),
    F_(dispatch_release), F_(dispatch_retain), F_(dispatch_once), F_(dispatch_once_f), F_(dispatch_async),
    F_(dispatch_get_global_queue), F_(OSAtomicAdd32Barrier), F_(OSAtomicAdd64Barrier),
    F_(OSAtomicCompareAndSwap32Barrier), F_(OSAtomicCompareAndSwapPtrBarrier), F_(OSAtomicCompareAndSwap64Barrier),
    F_(OSMemoryBarrier), F_(OSSpinLockLock), F_(OSSpinLockUnlock), F_(OSSpinLockTry), F_(os_unfair_lock_lock),
    F_(os_unfair_lock_unlock), F_(open), F_(close), F_(read), F_(write), F_(lseek), F_(pread), F_(fsync),
    F_(ftruncate), F_(fcntl), F_(flock), F_(isatty), F_(dup), F_(dup2), F_(unlink), F_(rmdir), F_(mkdir), F_(rename),
    F_(access), F_(chmod), F_(getcwd), F_(chdir), F_(realpath), F_(stat), F_(fstat), F_(opendir), F_(readdir),
    F_(closedir), F_(fopen), F_(fdopen), F_(fileno), F_(fclose), F_(fread), F_(fwrite), F_(fflush), F_(fseek),
    F_(fseeko), F_(ftell), F_(ftello), F_(fgets), F_(fputs), F_(fputc), F_(fgetc), F_(puts), F_(putchar), F_(feof),
    F_(ferror), F_(clearerr), F_(rewind), F_(setvbuf), F_(vsnprintf), F_(vsprintf), F_(vfprintf), F_(vprintf),
    F_(vasprintf), F_(snprintf), F_(sprintf), F_(fprintf), F_(printf), F_(asprintf), F_(__vsnprintf_chk),
    F_(__snprintf_chk), F_(__sprintf_chk), F_(sscanf), F_(vsscanf), F_(syslog), F_(mmap), F_(munmap), F_(mprotect),
    F_(madvise), F_(vm_allocate), F_(vm_read_overwrite), F_(_dyld_image_count), F_(_dyld_get_image_header),
    F_(_dyld_get_image_vmaddr_slide), F_(_dyld_get_image_name), F_(_dyld_register_func_for_add_image),
    F_(_dyld_register_func_for_remove_image), F_(getsectbynamefromheader_64), F_(dlopen), F_(dlsym), F_(dlclose),
    F_(dlerror), F_(dladdr), F_(sysctlbyname), F_(sysctl), F_(uname), F_(getrlimit), F_(setrlimit),
    F_(_NSGetExecutablePath), F_(_NSGetEnviron), F_(getprogname), F_(getpid), F_(getuid), F_(geteuid),
    F_(gethostname), F_(backtrace), F_(signal), F_(sigaction), F_(sigaltstack), F_(sigemptyset), F_(sigaddset),
    F_(sigfillset), F_(pthread_sigmask), F_(raise), F_(pthread_atfork), F_(pthread_setcancelstate),
    F_(pthread_setschedparam), F_(pthread_getschedparam), F_(sched_get_priority_max), F_(sched_get_priority_min),
    F_(_setjmp), F_(setjmp), F_(longjmp), F_(_longjmp), F_(__chkstk_darwin), F_(gettimeofday), F_(clock_gettime),
    F_(nanosleep), F_(localtime_r), F_(gmtime_r), F_(localtime), F_(gmtime), F_(mktime), F_(timegm), F_(strftime),
    F_(tzset), F_(clock), F_(setlocale), F_(newlocale), F_(uselocale), F_(freelocale), F_(localeconv), F_(__maskrune),
    F_(__tolower), F_(__toupper), F_(tolower), F_(toupper), F_(strlcat), F_(strncat), F_(strspn), F_(strcspn),
    F_(strpbrk), F_(strtok_r), F_(strsep), F_(strndup), F_(atol), F_(atof), F_(abs), F_(labs), F_(rand), F_(srand),
    F_(random), F_(srandom), F_(arc4random), F_(arc4random_uniform), F_(setenv), F_(unsetenv), F_(qsort), F_(bsearch),
    F_(acos), F_(acosf), F_(asin), F_(asinf), F_(atan), F_(atanf), F_(atan2), F_(atan2f), F_(cos), F_(cosf), F_(sin),
    F_(sinf), F_(tan), F_(tanf), F_(exp), F_(expf), F_(exp2f), F_(exp2), F_(fmod), F_(fmodf), F_(hypot), F_(log),
    F_(logf), F_(log10), F_(log10f), F_(log2f), F_(log2), F_(logb), F_(pow), F_(powf), F_(cosh), F_(sinh), F_(tanh),
    F_(coshf), F_(sinhf), F_(tanhf), F_(log1p), F_(expm1), F_(cbrt), F_(round), F_(roundf), F_(trunc), F_(truncf),
    F_(floor), F_(floorf), F_(ceil), F_(ceilf), F_(sqrt), F_(sqrtf), F_(rint), F_(rintf), F_(nearbyint),
    F_(remainder), F_(copysign), F_(copysignf), F_(asinh), F_(acosh), F_(atanh), F_(scalbn), F_(ldexp), F_(ldexpf),
    F_(frexp), F_(frexpf), F_(modf), F_(modff), F_(lround), F_(lroundf), F_(__exp10), F_(__exp10f),
    F_(__sincos_stret), F_(__sincosf_stret), F_(fegetenv), F_(fesetenv), F_(fegetround),
    { 0, 0 } };

mk_import_fn mk_sys_lookup(const char *sym) {
    for (const sys_entry *e = g_sys; e->name; e++)
        if (!strcmp(e->name, sym)) return e->fn;
    return NULL;
}
void mk_sys_init(void) { GetModuleFileNameA(NULL, g_exe_path, MAX_PATH); }
