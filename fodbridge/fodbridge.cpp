// fodbridge — safe, non-invasive FOD arm/disarm bridge for onyx (OnePlus port on POCO F7).
//
// The Xiaomi mfp-daemon FodEngine only lights Local-HBM when its state is armed
// (fingerprintConditionUpdate(4, {1,3,5})), reachable as IXiaomiFingerprint.extCmd(4,3).
// The OnePlus framework never sends that, so FOD stays dark. This daemon watches the
// HAL's OWN logcat and sends the proven extCmd arm/disarm sequence at auth start/stop.
//
// Safe by construction: only talks to IXiaomiFingerprint.extCmd (which never crashed);
// no IDisplayPanelFeature shim, no system_server involvement, no SystemUI/HAL patch.

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <sys/system_properties.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/input.h>
#include <stdlib.h>
#include <time.h>
#include "../gate.h"          // persist.sys.nischay.bomb master kill switch

#define LOGI(...) ((void)0)   // logging stripped for production

typedef AIBinder* (*pfn_check)(const char*);
static pfn_check p_checkService = nullptr;
static void loadDyn() {
    void* h = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
    if (h) p_checkService = (pfn_check)dlsym(h, "AServiceManager_checkService");
}

static const AIBinder_Class* xiaomiClass() {
    static const AIBinder_Class* c = []() -> const AIBinder_Class* {
        return AIBinder_Class_define(
            "vendor.xiaomi.hardware.fingerprintextension.IXiaomiFingerprint",
            [](void*) -> void* { return nullptr; }, [](void*) {},
            [](AIBinder*, transaction_code_t, const AParcel*, AParcel*) -> binder_status_t {
                return STATUS_UNKNOWN_TRANSACTION; });
    }();
    return c;
}

// extCmd(cmd, param): AIDL method id 1 on IXiaomiFingerprint is extCmd(int,int,out int).
static void extCmd(AIBinder* b, int32_t cmd, int32_t param) {
    AParcel* in = nullptr; AParcel* out = nullptr;
    if (AIBinder_prepareTransaction(b, &in) != STATUS_OK) return;
    AParcel_writeInt32(in, cmd);
    AParcel_writeInt32(in, param);
    AIBinder_transact(b, 1 /*FIRST_CALL_TRANSACTION = extCmd*/, &in, &out, 0);
    if (out) AParcel_delete(out);
}

static void controlFod(int action) {
    if (!p_checkService) return;
    AIBinder* b = p_checkService(
        "vendor.xiaomi.hardware.fingerprintextension.IXiaomiFingerprint/default");
    if (!b) return;
    if (!AIBinder_associateClass(b, xiaomiClass())) { AIBinder_decStrong(b); return; }
    if (action == 1) {   // ARM + illuminate-ready
        extCmd(b,10,1); extCmd(b,4,3); extCmd(b,7,1); extCmd(b,2,1);
        extCmd(b,1,1);  extCmd(b,3,2); extCmd(b,3,0);
    } else {             // DISARM
        extCmd(b,1,0); extCmd(b,2,0); extCmd(b,3,0); extCmd(b,7,0); extCmd(b,4,4);
    }
    AIBinder_decStrong(b);
    LOGI("controlFod(%d)", action);
}

// ---- Screen-off tap gestures: NOT handled here any more ---------------
// Double tap (touch mode 14) and single tap (mode 16) are owned by the corrected
// IOplusTouch HAL (thermal/hal-rewrite/touch), driven by OnePlus's own framework
// through IOplusTouch nodes 1/21. This daemon used to arm mode 14 from
// `oplus_customize_gesture_double_touch`, a key that no longer tracks the
// Settings toggle (it read 0 while double_tap_to_wake=1), and on every screen-off
// it overwrote the HAL -- the IC suspended with "Disable doubletap gesture".

static bool isArmStart(const char* line) {
    if (!strstr(line, "mfp_service")) return false;
    // ignore non-session calls
    if (strstr(line, "resetLockout") || strstr(line, "generateChallenge") ||
        strstr(line, "revokeChallenge")) return false;
    // a FOD session starts on authenticate (keyguard) or enroll (settings)
    return strstr(line, "authenticate") || strstr(line, "enroll");
}
static bool isDisarm(const char* line) {
    if (!strstr(line, "mfp_service")) return false;
    // Real session end: framework cancel/error, or permanent lockout.
    if (strstr(line, "Session cancel") || strstr(line, "onError") ||
        strstr(line, "onLockoutPermanent"))
        return true;
    // Successful unlock: onAuthenticated with a NON-zero fid.
    // A FAILED match is onAuthenticated(fid=0) -> do NOT disarm; the session is
    // still active and retries must keep HBM working.
    if (strstr(line, "onAuthenticated(fid=") && !strstr(line, "onAuthenticated(fid=0)"))
        return true;
    return false;
}

// ---- Screen-off FOD: no forced wake -------------------------------------
// This daemon used to wake the panel (KEY_WAKEUP) while a finger was on the FOD
// in doze, so Local-HBM would be bright enough to capture, and put it back to
// sleep after a failed press. Removed per user: touching the FOD must not switch
// the lock screen on. Screen-off / AOD fingerprint is left to the native
// OnePlus + mfp-daemon flow.

extern "C" void aoddoze_press(int on);  // bright-doze during a FOD press (aoddoze.cpp)

// ---- Instant FOD-down watcher (for screen-off capture) ------------------
// The HAL's onAcquired(22) reaches us through a logcat pipe (~100-300 ms late),
// by which time the AOD panel has already cycled into DOZE_SUSPEND and the
// capture aborts GF_ERROR_TOO_FAST. The NT38771 emits BTN_TOOL_FINGER (338) the
// instant the finger lands on the FOD (nvt_ts_fod_down_report), even in doze, so
// we poll the touch input device directly (0 lag) and hold bright doze from that
// instant -- keeping the panel in DOZE (not DOZE_SUSPEND) for the whole capture.
static void* fodInputWatcher(void*) {
    char path[64] = {0};
    for (int i = 0; i < 32; i++) {
        char p[64]; snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char name[128] = {0};
        bool hit = ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
                   strcmp(name, "NVTCapacitiveTouchScreen") == 0;
        close(fd);
        if (hit) { snprintf(path, sizeof(path), "%s", p); break; }
    }
    if (!path[0]) { LOGI("fodInputWatcher: NVT device not found"); return nullptr; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    LOGI("fodInputWatcher: watching %s for FOD key 338", path);
    struct input_event ev;
    while (true) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) { if (n < 0 && errno == EINTR) continue; break; }
        if (ev.type == EV_KEY && ev.code == BTN_TOOL_FINGER)   // 338: FOD down/up
            aoddoze_press(ev.value && bombEnabled() ? 1 : 0);
    }
    close(fd);
    return nullptr;
}

// Process one HAL log line: arm/disarm the FodEngine and mirror finger down/up.
static void processLine(const char* line, int& armed, int& press) {
    // Master kill switch: when disabled, never arm the FodEngine (no illumination)
    // and never publish finger events. If we were armed, disarm cleanly and clear.
    if (!bombEnabled()) {
        if (armed != 0) { armed = 0; controlFod(0); }
        if (press != 0) { press = 0; __system_property_set("sys.bomb.fod", "0"); }
        return;
    }
    if (isArmStart(line)) {
        if (armed != 1) { armed = 1; controlFod(1); }
    } else if (isDisarm(line)) {
        if (armed != 0) { armed = 0; controlFod(0); }
        if (press != 0) { press = 0; __system_property_set("sys.bomb.fod", "0"); }
    }
    // Mirror finger down/up so libfodhook (inside SystemUI) can drive the
    // pressed animation / Quick Launch via onFpTouch. 22 = down, 23 = up.
    if (strstr(line, "mfp_service") && strstr(line, "onAcquired(7) (22)")) {
        if (press != 1) {
            press = 1;
            __system_property_set("sys.bomb.fod", "1");
        }
    } else if (strstr(line, "mfp_service") && strstr(line, "onAcquired(7) (23)")) {
        if (press != 0) { press = 0; __system_property_set("sys.bomb.fod", "0"); }
    }
}

static int mfpDaemonPid() {
    FILE* p = popen("pidof mfp-daemon", "r");
    if (!p) return -1;
    char b[32] = {0};
    char* r = fgets(b, sizeof(b), p);
    pclose(p);
    return r ? atoi(b) : -1;
}

// A pollable fd that becomes readable when `pid` exits, so we detect a mfp-daemon
// restart event-driven (kernel 5.3+) instead of waking every 30s to re-check.
static int pidfdOpen(int pid) {
#ifdef SYS_pidfd_open
    return (int)syscall(SYS_pidfd_open, pid, 0u);
#else
    return -1;
#endif
}

extern "C" void aoddoze_start();   // AOD keep-alive driver (aoddoze.cpp)
extern "C" void aoddoze_press(int on);  // bright-doze during a FOD press (aoddoze.cpp)

int main() {
    loadDyn();
    LOGI("fodbridge started");
    // Create sys.bomb.fod up front so libfodhook can __system_property_wait on it
    // immediately (event-driven, 0% idle CPU) instead of polling until it exists.
    __system_property_set("sys.bomb.fod", "0");
    // Start the AOD keep-alive driver (forces panel doze_brightness on at each
    // doze entry so AOD clocks stay lit instead of going dark after the anim).
    aoddoze_start();
    pthread_t fw; pthread_create(&fw, nullptr, fodInputWatcher, nullptr); pthread_detach(fw);

    int armed = -1;   // -1 unknown, 0 disarmed, 1 armed
    int press = -1;   // finger down/up mirror for sys.bomb.fod (libfodhook -> onFpTouch)

    // Tail ONLY the fingerprint HAL's logs (logd filters by pid server-side), so
    // fodbridge wakes only when mfp-daemon actually logs — which at idle is ~never.
    // We block on poll() forever (no periodic wakeup): the log pipe wakes us on new
    // lines, and a pidfd on the HAL wakes us the instant it restarts, so FOD keeps
    // working without a reboot. Fully event-driven -> 0% CPU at idle.
    while (true) {
        int mfp = mfpDaemonPid();
        if (mfp <= 0) { sleep(2); continue; }
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "logcat -v brief -T 1 --pid=%d", mfp);
        FILE* f = popen(cmd, "r");
        if (!f) { sleep(2); continue; }
        int lfd = fileno(f);
        int pfd = pidfdOpen(mfp);            // signals when the HAL exits

        std::string acc;
        char rbuf[4096];
        bool respawn = false;
        while (!respawn) {
            struct pollfd fds[2] = { { lfd, POLLIN, 0 }, { pfd, POLLIN, 0 } };
            int nfds = (pfd >= 0) ? 2 : 1;
            // Block at 0% CPU: forever when we can watch the pidfd, else fall back
            // to a 30s re-check for the (unexpected) no-pidfd kernel.
            int pr = poll(fds, nfds, pfd >= 0 ? -1 : 30000);
            if (pr < 0) break;
            if (pr == 0) { if (mfpDaemonPid() != mfp) respawn = true; continue; }
            if (pfd >= 0 && (fds[1].revents & (POLLIN | POLLHUP))) { respawn = true; break; }
            if (fds[0].revents & (POLLHUP | POLLERR)) break;   // logcat ended
            if (fds[0].revents & POLLIN) {
                ssize_t n = read(lfd, rbuf, sizeof(rbuf));
                if (n <= 0) break;
                acc.append(rbuf, (size_t)n);
                size_t nl;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    processLine(acc.substr(0, nl).c_str(), armed, press);
                    acc.erase(0, nl + 1);
                }
            }
        }
        if (pfd >= 0) close(pfd);
        pclose(f);
    }
    return 0;
}
