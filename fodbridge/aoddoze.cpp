// aoddoze.cpp — AOD keep-alive driver (separate module, linked into fodbridge).
//
// On this OnePlus-on-Xiaomi port the compositor never sets the Xiaomi panel's
// doze_brightness, so when the screen enters AOD it plays the entry animation
// and then goes dark (the panel drops to doze_suspend unlit). The fix (proven,
// and exactly what libhoshikv's aod_doze_thread does) is to force the panel's
// doze_brightness on at every doze entry via the Xiaomi mi_disp ioctl:
//
//   ioctl(fd, MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS, &{flag=0, disp_id=0, value})
//   MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS = _IOW('D', 2, 12) = 0x400c4402
//   value 1 = AOD low-brightness on, 0 = off.
//
// /dev/mi_display/disp_feature is crw-rw-rw- (world-writable) + SELinux permissive,
// so this needs no special privilege. A single successful write per doze entry
// holds through doze_suspend (verified on-device), so it is fully event-driven
// (__system_property_wait, 0% CPU when idle) — no polling, no battery cost.
// The ioctl returns EINVAL if fired the instant the panel begins its doze
// transition, so we retry briefly until the panel accepts it.

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include "../gate.h"          // persist.sys.nischay.bomb master kill switch

#define LOGI(...) ((void)0)   // logging stripped for production

#define MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS 0x400c4402
struct doze_req { uint32_t flag; uint32_t disp_id; uint32_t value; };


static int g_dispFd = -1;

static int dispFd() {
    if (g_dispFd < 0) g_dispFd = open("/dev/mi_display/disp_feature", O_RDWR);
    return g_dispFd;
}

// One SET_DOZE_BRIGHTNESS, no retry. Returns true on success.
static bool applyDozeOnce(int val) {
    int fd = dispFd();
    if (fd < 0) return false;
    struct doze_req r = { 0, 0, (uint32_t)val };
    return ioctl(fd, MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS, &r) == 0;
}

// Set doze_brightness, retrying until the panel accepts it (it returns EINVAL
// right at the doze transition until it is ready). Returns true on success.
static bool applyDoze(int val) {
    if (dispFd() < 0) { LOGI("cannot open disp_feature"); return false; }
    for (int i = 0; i < 25; i++) {                      // up to ~3.7s
        if (applyDozeOnce(val)) return true;
        usleep(150 * 1000);
    }
    return false;
}

static bool inDoze() {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.tracing.screen_state", v);
    int st = atoi(v);
    return st == 1 || st == 3 || st == 4;   // any doze sub-state (not off/on)
}

// ---- Screen-off FOD capture, invisibly (no wake) ------------------------
// LHBM lights brightly enough for the optical sensor to capture in AOD only when
// the panel is at bright doze (doze_brightness 2); at level 1 the finger "doesn't
// register" and at the transient state-1 (panel momentarily dark between AOD
// paints) it fails too. So while a finger is on the FOD, hold the panel at bright
// doze and KEEP re-asserting it through the doze state cycling. The panel never
// leaves doze -> no visible wake, no lock screen (unlike the old keyevent wake).
// Event-driven: the holder thread only spins during an actual press (~1s), then
// the panel returns to normal AOD (level 1). 0% idle CPU.
static volatile int g_press = 0;

static void* pressHolder(void*) {
    while (g_press && bombEnabled()) {
        if (inDoze()) applyDozeOnce(2);
        usleep(60 * 1000);
    }
    if (inDoze()) applyDoze(1);            // restore normal AOD brightness
    return nullptr;
}

// Called by fodbridge on finger down (on=1) / up (on=0). Only acts in doze; a
// press while the screen is already on is left entirely to the normal flow.
extern "C" void aoddoze_press(int on) {
    if (on) {
        if (!inDoze() || g_press) return;
        g_press = 1;
        applyDozeOnce(2);                  // light immediately for the capture
        pthread_t t; pthread_create(&t, nullptr, pressHolder, nullptr); pthread_detach(t);
    } else {
        g_press = 0;                       // pressHolder restores level 1 and exits
    }
}

static void* aodWorker(void*) {
    const prop_info* pi = __system_property_find("debug.tracing.screen_state");
    uint32_t serial = 0;   // 0 => first wait returns at once; then track real serial
    int prev = -1;
    LOGI("aoddoze started");
    while (true) {
        if (!pi) { sleep(1); pi = __system_property_find("debug.tracing.screen_state"); continue; }
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.tracing.screen_state", v);
        int st = atoi(v);                              // 0=off 1=doze 2=on 3=doze_suspend
        bool wasDoze = (prev == 1 || prev == 3);
        bool isDoze  = (st == 1 || st == 3);
        // Master kill switch: when disabled, don't drive the panel at all (no AOD
        // keep-alive). Track state so behavior resumes cleanly when re-enabled.
        if (!bombEnabled()) { prev = st; uint32_t ns0 = serial;
            __system_property_wait(pi, serial, &ns0, nullptr); serial = ns0; continue; }
        // Hold AOD at BRIGHT doze (level 2), re-applied on every doze sub-state
        // transition (DOZE<->DOZE_SUSPEND cycles ~1/s). This keeps the NT38771
        // touch in normal continuous-report mode so a FOD press reads as a held
        // press (not a brief gesture tap) and LHBM can capture -- invisibly, in
        // AOD, with no wake to the lock screen. Event-driven off screen_state:
        // 0% idle CPU between transitions (no polling). The finger-down burst in
        // aoddoze_press() then pins it through the actual capture.
        if (isDoze) {
            applyDoze(2);
        } else if (st == 2 && prev != 2) {             // screen on -> clear
            applyDoze(0);
        }
        (void)wasDoze;
        prev = st;
        uint32_t ns = serial;
        __system_property_wait(pi, serial, &ns, nullptr);   // block (0% CPU) until change
        serial = ns;
    }
    return nullptr;
}

// Spawn the AOD keep-alive thread. Called from fodbridge main().
extern "C" void aoddoze_start() {
    pthread_t t;
    pthread_create(&t, nullptr, aodWorker, nullptr);
    pthread_detach(t);
}
