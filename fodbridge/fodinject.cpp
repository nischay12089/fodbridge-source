// fodinject — minimal ARM64 ptrace .so injector.
// Injects /system/lib64/libfodhook.so into com.android.systemui by remotely
// calling dlopen() in the target. Runs as root (Magisk). Re-injects if SystemUI
// restarts. libfodhook lives in /system/lib64 so SystemUI's linker namespace
// permits the dlopen path.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <poll.h>
#include "../gate.h"          // persist.sys.nischay.bomb master kill switch

#define LOGI(...) ((void)0)   // logging stripped for production
#define LOGE(...) ((void)0)

static const char* TARGET = "com.android.systemui";
static const char* SOPATH = "/system/lib64/libfodhook.so";

// Block (0% CPU) until a pid exits: pidfd_open + poll(POLLIN). SystemUI restarts
// are rare, so this replaces a 24/7 30s /proc poll with a single blocking wait
// that also re-injects INSTANTLY on restart instead of up to 30s later.
static int pidfd_open_compat(pid_t pid) {
#ifdef SYS_pidfd_open
    return (int)syscall(SYS_pidfd_open, pid, 0u);
#else
    return -1;
#endif
}

struct regs64 { uint64_t regs[31]; uint64_t sp; uint64_t pc; uint64_t pstate; };

static int findPid(const char* name) {
    DIR* d = opendir("/proc");
    if (!d) return -1;
    struct dirent* e; int pid = -1;
    while ((e = readdir(d))) {
        int p = atoi(e->d_name);
        if (p <= 0) continue;
        char path[64], buf[256];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", p);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strcmp(buf, name) == 0) { pid = p; break; }
    }
    closedir(d);
    return pid;
}

// lowest mapped address of a library (soname substring) in a process's maps
static uintptr_t libBase(int pid, const char* soname) {
    char path[64];
    if (pid == 0) snprintf(path, sizeof(path), "/proc/self/maps");
    else snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512]; uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, soname)) { base = (uintptr_t)strtoull(line, nullptr, 16); break; }
    }
    fclose(f);
    return base;
}

static bool alreadyInjected(int pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r"); if (!f) return false;
    char line[512]; bool yes = false;
    while (fgets(line, sizeof(line), f)) { if (strstr(line, "libfodhook.so")) { yes = true; break; } }
    fclose(f);
    return yes;
}

static bool getRegs(int pid, regs64* r) {
    struct iovec iov = { r, sizeof(*r) };
    return ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == 0;
}
static bool setRegs(int pid, regs64* r) {
    struct iovec iov = { r, sizeof(*r) };
    return ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov) == 0;
}

static bool writeMem(int pid, uintptr_t dst, const void* src, size_t len) {
    struct iovec liov = { (void*)src, len };
    struct iovec riov = { (void*)dst, len };
    return process_vm_writev(pid, &liov, 1, &riov, 1, 0) == (ssize_t)len;
}

static bool readMem(int pid, uintptr_t src, void* dst, size_t len) {
    struct iovec liov = { dst, len };
    struct iovec riov = { (void*)src, len };
    return process_vm_readv(pid, &liov, 1, &riov, 1, 0) == (ssize_t)len;
}

// remote-call a function: set x0..x2, pc=func, lr=0 (return-trap -> SIGSEGV), run.
static uint64_t remoteCall(int pid, uintptr_t func, uint64_t a0, uint64_t a1, uint64_t a2, regs64 saved) {
    regs64 r = saved;
    r.regs[0] = a0;
    r.regs[1] = a1;
    r.regs[2] = a2;
    r.regs[30] = 0;                     // lr = 0 -> fault on return, we catch it
    r.pc = func;
    r.sp = (saved.sp - 0x800) & ~0xFUL; // scratch + 16-byte aligned
    if (!setRegs(pid, &r)) { LOGE("setregs(call) failed"); return 0; }
    ptrace(PTRACE_CONT, pid, 0, 0);
    int st; waitpid(pid, &st, 0);
    // expect stop by SIGSEGV (returned to 0). Read x0.
    regs64 out; getRegs(pid, &out);
    return out.regs[0];
}

// Same, with a 4th argument (x3) — for __loader_android_dlopen_ext(name,flags,info,caller).
static uint64_t remoteCall4(int pid, uintptr_t func, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3, regs64 saved) {
    regs64 r = saved;
    r.regs[0] = a0; r.regs[1] = a1; r.regs[2] = a2; r.regs[3] = a3;
    r.regs[30] = 0;
    r.pc = func;
    r.sp = (saved.sp - 0x800) & ~0xFUL;
    if (!setRegs(pid, &r)) { LOGE("setregs(call4) failed"); return 0; }
    ptrace(PTRACE_CONT, pid, 0, 0);
    int st; waitpid(pid, &st, 0);
    regs64 out; getRegs(pid, &out);
    return out.regs[0];
}

// Write the whole file at `path` into the memfd SystemUI opened at fd `rfd`,
// reached from root via /proc/<pid>/fd/<rfd> (a magic symlink to the same memfd).
static bool writeFileToRemoteFd(int pid, int rfd, const char* path) {
    int src = open(path, O_RDONLY);
    if (src < 0) return false;
    char dst[64]; snprintf(dst, sizeof(dst), "/proc/%d/fd/%d", pid, rfd);
    int out = open(dst, O_WRONLY);
    if (out < 0) { close(src); return false; }
    char buf[65536]; ssize_t n; bool ok = true;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        ssize_t o = 0;
        while (o < n) { ssize_t w = write(out, buf + o, n - o); if (w <= 0) { ok = false; break; } o += w; }
        if (!ok) break;
    }
    close(src); close(out);
    return ok;
}

static bool inject(int pid) {
    regs64 saved;
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) != 0) { LOGE("attach failed"); return false; }
    int st; waitpid(pid, &st, 0);
    if (!getRegs(pid, &saved)) { LOGE("getregs failed"); ptrace(PTRACE_DETACH, pid, 0, 0); return false; }

    // Resolve linker/libc entry points by base-diff (same binaries in both procs).
    uintptr_t l_lk = libBase(0, "linker64"), r_lk = libBase(pid, "linker64");
    uintptr_t r_libc = libBase(pid, "/libc.so"), l_libc = libBase(0, "/libc.so");
    void* hdl = dlopen("libdl.so", RTLD_NOW);
    void* hlc = dlopen("libc.so",  RTLD_NOW);
    uintptr_t l_loader    = hdl ? (uintptr_t)dlsym(hdl, "__loader_dlopen") : 0;
    uintptr_t l_loaderext = hdl ? (uintptr_t)dlsym(hdl, "__loader_android_dlopen_ext") : 0;
    uintptr_t l_memfd     = hlc ? (uintptr_t)dlsym(hlc, "memfd_create") : 0;
    uintptr_t r_loader    = (l_loader    && l_lk && r_lk)   ? (l_loader    - l_lk   + r_lk)   : 0;
    uintptr_t r_loaderext = (l_loaderext && l_lk && r_lk)   ? (l_loaderext - l_lk   + r_lk)   : 0;
    uintptr_t r_memfd     = (l_memfd && l_libc && r_libc)   ? (l_memfd     - l_libc + r_libc) : 0;
    uintptr_t l_base = libBase(0, "/libdl.so"), r_base = libBase(pid, "/libdl.so");
    uintptr_t r_dlopen  = (l_base && r_base) ? ((uintptr_t)&dlopen  - l_base + r_base) : 0;
    uintptr_t r_dlerror = (l_base && r_base) ? ((uintptr_t)&dlerror - l_base + r_base) : 0;
    LOGI("loaderext=%p memfd=%p libc=%p linker=%p", (void*)r_loaderext, (void*)r_memfd, (void*)r_libc, (void*)r_lk);

    // Remote scratch below saved sp (all above the remote-call sp = saved.sp-0x800).
    uintptr_t strAddr  = (saved.sp - 0x400) & ~0xFUL;   // the .so path (for name)
    uintptr_t infoAddr = (saved.sp - 0x200) & ~0xFUL;   // android_dlextinfo (48 bytes)
    uintptr_t nameAddr = (saved.sp - 0x600) & ~0xFUL;   // memfd name
    if (!writeMem(pid, strAddr, SOPATH, strlen(SOPATH) + 1)) { LOGE("writeMem path failed"); ptrace(PTRACE_DETACH, pid, 0, 0); return false; }

    uint64_t handle = 0;

    // PRIMARY: memfd inject — no file needs to be visible on /system. Under KernelSU
    // the module overlay isn't in SystemUI's mount namespace, so a path dlopen gets
    // "not found". Instead: create a memfd in SystemUI, fill it with libfodhook.so
    // from root via /proc/<pid>/fd/N, then android_dlopen_ext(USE_LIBRARY_FD) with
    // name=/system/lib64/libfodhook.so — its dir is in the namespace search paths so
    // the accessibility check passes, and the content comes from the fd (no lookup).
    if (r_memfd && r_loaderext) {
        writeMem(pid, nameAddr, "fh", 3);
        uint64_t fd = remoteCall(pid, r_memfd, nameAddr, 1 /*MFD_CLOEXEC*/, 0, saved);
        LOGI("remote memfd_create = %ld", (long)(int)fd);
        if ((int)fd >= 0 && writeFileToRemoteFd(pid, (int)fd, SOPATH)) {
            uint8_t info[48] = {0};
            uint64_t flags = 0x10;   // ANDROID_DLEXT_USE_LIBRARY_FD
            int32_t  libfd = (int32_t)fd;
            memcpy(info + 0, &flags, 8);
            memcpy(info + 28, &libfd, 4);   // android_dlextinfo.library_fd
            writeMem(pid, infoAddr, info, sizeof(info));
            handle = remoteCall4(pid, r_loaderext, strAddr, 2 /*RTLD_NOW*/, infoAddr, r_libc, saved);
            LOGI("remote __loader_android_dlopen_ext(memfd) = %p", (void*)handle);
        } else LOGE("memfd fill failed (fd=%ld)", (long)(int)fd);
    }
    // FALLBACK: plain file-path dlopen (Magisk magic-mount setups where /system is
    // visible to the app).
    if (!handle && r_loader && r_libc) {
        handle = remoteCall(pid, r_loader, strAddr, 2, r_libc, saved);
        LOGI("remote __loader_dlopen(caller=libc) = %p", (void*)handle);
    }
    if (!handle && r_dlopen) {
        handle = remoteCall(pid, r_dlopen, strAddr, 2, 0, saved);
        LOGI("remote dlopen(caller=0) = %p", (void*)handle);
    }
    if (!handle && r_dlerror) {   // DEBUG: read the linker's rejection reason
        uint64_t errp = remoteCall(pid, r_dlerror, 0, 0, 0, saved);
        char buf[300] = {0};
        if (errp && readMem(pid, (uintptr_t)errp, buf, sizeof(buf) - 1))
            LOGE("remote dlerror: %s", buf);
        else LOGE("remote dlerror: (null/unreadable errp=%p)", (void*)errp);
    }

    // restore original state and let SystemUI continue
    setRegs(pid, &saved);
    ptrace(PTRACE_DETACH, pid, 0, 0);

    return handle != 0;
}

int main(int argc, char** argv) {
    // Test mode: fodinject <pid> <sopath>  -> inject once, exit (for validation).
    if (argc >= 3) {
        int pid = atoi(argv[1]);
        SOPATH = argv[2];
        LOGI("test inject %s into pid %d", SOPATH, pid);
        bool ok = inject(pid);
        LOGI("test inject %s", ok ? "OK" : "FAILED");
        printf("inject %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }

    // wait for boot + SystemUI + fingerprint HAL
    for (int i = 0; i < 60; i++) {
        char v[16] = {0};
        FILE* p = popen("getprop sys.boot_completed", "r");
        if (p) { if (fgets(v, sizeof(v), p)) {} pclose(p); }
        if (v[0] == '1') break;
        sleep(2);
    }
    sleep(10);

    while (true) {
        // Master kill switch: don't inject unless enabled. Blocks at 0% CPU until
        // persist.sys.nischay.bomb becomes "1"; libfodhook self-gates on the same
        // prop, so if it turns off after injection the lib simply goes inert.
        waitBombEnabled();
        int pid = findPid(TARGET);
        if (pid <= 0) { sleep(5); continue; }        // SystemUI not up yet
        if (!alreadyInjected(pid)) {
            LOGI("injecting into %s (pid %d)", TARGET, pid);
            bool ok = inject(pid);
            LOGI("inject %s", ok ? "OK" : "FAILED");
            if (!ok) { sleep(5); continue; }          // transient failure -> retry soon
        }
        // Fully event-driven: sleep at 0% CPU until THIS SystemUI process exits,
        // then loop to find + inject the replacement. No periodic /proc polling.
        int pfd = pidfd_open_compat(pid);
        if (pfd >= 0) {
            struct pollfd p = { pfd, POLLIN, 0 };
            poll(&p, 1, -1);                          // blocks until SystemUI dies
            close(pfd);
            LOGI("%s (pid %d) exited -> re-inject", TARGET, pid);
        } else {
            sleep(60);   // kernel without pidfd_open: gentle fallback re-check
        }
    }
    return 0;
}
