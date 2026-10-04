// gate.h — master protection gate for the whole FOD module.
//
// EVERY feature (FOD illumination/arming, screen-off wake, AOD keep-alive, pressed
// animation, Quick Launch, SystemUI injection) is fully disabled unless the gate is
// ON, which means the read-only property:
//
//   ro.sys.nischay.bomb == "1"
//
// `ro.` is set ONCE by init from build.prop and can NEVER be changed by /data or
// setprop. So baking `ro.sys.nischay.bomb=1` into the ROM's build.prop makes the
// module work BY DEFAULT on a fresh flash (no setprop, no root manager); removing the
// line — or any value other than "1" — disables everything ("otherwise no"). The gate
// is controlled purely by the ROM's build.prop and nothing at runtime can flip it.
//
// The check is live (daemons re-read it per event). fodinject blocks (0% CPU) until
// enabled before it injects. In the partition/ROM install, fodfix.rc also gates the
// init services on ro.sys.nischay.bomb=1 so nothing even starts unless the ROM enables
// it in build.prop.
#pragma once
#include <unistd.h>
#include <sys/system_properties.h>

#define BOMB_GATE_PROP "ro.sys.nischay.bomb"   // build.prop protection gate

// True only when the gate is explicitly "1". Missing / "0" / anything else = off.
static inline bool bombEnabled() {
    char v[PROP_VALUE_MAX] = {0};
    if (__system_property_get(BOMB_GATE_PROP, v) <= 0) return false;  // absent
    return v[0] == '1' && v[1] == '\0';
}

// Block at 0% CPU until the gate is enabled (returns at once if already on). The gate
// is read-only and fixed at boot, so if it is on this returns immediately; if it is
// off it never turns on, and the caller (fodinject) simply never proceeds — which is
// the intended "disabled" behavior.
static inline void waitBombEnabled() {
    const prop_info* pi = __system_property_find(BOMB_GATE_PROP);
    uint32_t serial = 0;
    while (!bombEnabled()) {
        if (!pi) { sleep(1); pi = __system_property_find(BOMB_GATE_PROP); continue; }
        uint32_t ns = serial;
        __system_property_wait(pi, serial, &ns, nullptr);
        serial = ns;
    }
}
