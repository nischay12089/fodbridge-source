# fodbridge

A small, **non-invasive** fingerprint/FOD bridge for the **POCO F7 (onyx)** running a
**OnePlus OxygenOS (Android)** system port on top of the Xiaomi vendor stack.

On this port the OnePlus framework never arms Xiaomi's under-display fingerprint the way
the stock Xiaomi framework does, so the in-display fingerprint (FOD) stays dark. fodbridge
fixes that from userspace **without patching SystemUI, the fingerprint HAL, or any APK/jar**
— it only talks to the HAL's own Xiaomi extension interface and a couple of display nodes.

> Note: this is the earlier, userspace-daemon approach. It has since been superseded by a
> from-scratch native AIDL fingerprint HAL (mfp-daemon/oplusfp) that does the same job
> inside the HAL process. fodbridge is published here for reference/history.

## Components

| Binary | Source | What it does |
|---|---|---|
| `fodbridge` | `fodbridge.cpp` + `aoddoze.cpp` | Root daemon. Watches the fingerprint HAL's own logcat and, at auth start/stop, sends the proven `IXiaomiFingerprint.extCmd(4, 3)` arm / disarm sequence so Local-HBM lights. `aoddoze` keeps the always-on display lit by driving the Xiaomi `mi_disp` doze-brightness ioctl on doze entry (event-driven, 0% idle). |
| `fodinject` | `fodinject.cpp` | Minimal ARM64 `ptrace` injector that loads `libfodhook.so` into `com.android.systemui` (remote `dlopen`) to drive the pressed ripple + Quick Launch, re-injecting if SystemUI restarts. |
| `libfodhook.so` | `fodhook/libfodhook.cpp` | The payload `fodinject` loads into `com.android.systemui`. Grabs the live `JavaVM` and drives the OnePlus FOD press UI via JNI: polls `sys.bomb.fod` (published by `fodbridge` on finger down/up) and calls `OnScreenFingerprintUiMech.onFpTouch(boolean)` by reflection — the pressed ripple + Quick Launch, with no APK/jar patch. |
| `fodprobe` | `fodprobe.cpp` | Root probe harness to iterate the doze-FOD illumination sequence live (reads commands from `/data/local/tmp/fodprobe.cmd`) without reflashing. Dev tool, not part of the runtime. |

## Master gate — `gate.h`

Every feature is disabled unless the read-only build.prop gate is on:

```
ro.sys.nischay.bomb=1
```

`ro.` is set once by init from `build.prop` and can never be changed at runtime, so baking
the line into the ROM enables the module by default on a fresh flash; removing it (or any
other value) disables everything. Daemons re-read it live; `fodinject` blocks at 0% CPU
until it is enabled.

## Build

Needs the Android NDK (tested with r26). Each module builds from its own folder:

```bash
cd fodbridge && ndk-build   # -> libs/arm64-v8a/{fodbridge, fodinject}
cd fodhook   && ndk-build   # -> libs/arm64-v8a/libfodhook.so
```

The binaries link the device's `libbinder_ndk` and run as root (e.g. from a Magisk/
KernelSU service); `libfodhook.so` goes to `/system/lib64` so SystemUI's linker namespace
allows the `dlopen`. `fodprobe` is built separately when needed. Both modules include
`../gate.h` from the repo root, so keep the layout intact.

## Layout

```
gate.h                 master build.prop gate (included as ../gate.h)
fodbridge/
├── fodbridge.cpp      arm/disarm bridge (logcat-driven)
├── aoddoze.cpp        AOD keep-alive (mi_disp doze brightness)
├── fodinject.cpp      ptrace libfodhook injector
├── fodprobe.cpp       root probe harness (dev)
└── jni/{Android.mk, Application.mk}
fodhook/
├── libfodhook.cpp     SystemUI-side FOD press UI driver (JNI)
└── jni/{Android.mk, Application.mk}
```
