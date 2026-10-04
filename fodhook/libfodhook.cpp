// libfodhook.so — injected into com.android.systemui via ptrace (see fodinject).
// Once inside, it grabs the live JavaVM and drives the OnePlus FOD press UI:
// it polls the prop `sys.bomb.fod` (published by fodbridge on finger down/up)
// and calls OnScreenFingerprintUiMech.onFpTouch(boolean) via JNI — the same
// thing your friend's in-SystemUI `bomb` thread did, but with no APK patch.
//
// Reflection chain (all resolvable at runtime, JNI ignores private access):
//   DependencyEx.sDependency (static)
//     .getDependency(OplusKeyguardDependencyEx.class)
//     .getOplusBiometricAuthController()
//     .onScreenFingerprintUiMech            (private field)
//     .onFpTouch(boolean)

#include <jni.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include "../gate.h"          // persist.sys.nischay.bomb master kill switch

#define LOGI(...) ((void)0)   // logging stripped for production
#define LOGE(...) ((void)0)

typedef jint (*GetVMs_t)(JavaVM**, jsize, jsize*);

static JavaVM* getVm() {
    // JNI_GetCreatedJavaVMs is exported by libnativehelper (fallbacks: libart, libandroid_runtime)
    const char* libs[] = {"libnativehelper.so", "libart.so", "libandroid_runtime.so"};
    for (const char* n : libs) {
        void* h = dlopen(n, RTLD_NOW | RTLD_GLOBAL);
        if (!h) continue;
        GetVMs_t f = (GetVMs_t)dlsym(h, "JNI_GetCreatedJavaVMs");
        if (!f) continue;
        JavaVM* vm = nullptr; jsize n2 = 0;
        if (f(&vm, 1, &n2) == JNI_OK && n2 > 0 && vm) { LOGI("JavaVM via %s", n); return vm; }
    }
    return nullptr;
}

// ---- shared state for the two event-driven worker threads --------------
static JavaVM*   s_vm        = nullptr;
static jobject   s_mech      = nullptr;   // OnScreenFingerprintUiMech (global ref)
static jmethodID s_onFpTouch = nullptr;   // onFpTouch(Z)V
static jobject   s_qlm       = nullptr;   // QuickLaunchManager (global ref)
static jmethodID s_qlIcon    = nullptr;   // notifyFpIconAppear()V
static jmethodID s_qlReg     = nullptr;   // registerInputEvent()V
static jmethodID s_qlReset   = nullptr;   // notifyFpTouchDown()V

// Ensure Quick Launch is wired (container+callback), the gesture monitor is
// registered, and the sticky touchUp flag is cleared. All three are idempotent
// and self-gated (isQuickLaunchAppEnable + keyguard-showing), so they're cheap
// no-ops off the lock screen. Called only on real events — never polled.
static void qlSetup(JNIEnv* env) {
    if (!s_qlm || !bombEnabled()) return;   // kill switch: no Quick Launch upkeep when off
    env->CallVoidMethod(s_qlm, s_qlIcon);  if (env->ExceptionCheck()) env->ExceptionClear();
    env->CallVoidMethod(s_qlm, s_qlReg);   if (env->ExceptionCheck()) env->ExceptionClear();
    env->CallVoidMethod(s_qlm, s_qlReset); if (env->ExceptionCheck()) env->ExceptionClear();
}

// Screen-state watcher. Blocks (0% CPU) on debug.tracing.screen_state and, when
// the display turns fully on (SurfaceFlinger power mode "2"), preps Quick Launch
// for the lock-screen session BEFORE the user can press. No polling.
static void* screenWorker(void*) {
    if (!s_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (s_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return nullptr;
    qlSetup(env);                                    // handle "screen already on at load"
    const prop_info* pi = __system_property_find("debug.tracing.screen_state");
    uint32_t serial = 0;   // 0 => first wait returns at once; then we track the real serial
    char last[PROP_VALUE_MAX] = {0};
    while (true) {
        if (!pi) { usleep(1000 * 1000); pi = __system_property_find("debug.tracing.screen_state");
                   serial = 0; continue; }
        uint32_t ns = serial;
        __system_property_wait(pi, serial, &ns, nullptr);   // block until it changes
        serial = ns;
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.tracing.screen_state", v);
        if (strcmp(v, "2") == 0 && strcmp(last, "2") != 0) qlSetup(env);
        strncpy(last, v, sizeof(last) - 1);
    }
    return nullptr;
}

static void* worker(void*) {
    // give ART a moment after injection
    usleep(500 * 1000);
    JavaVM* vm = getVm();
    if (!vm) { LOGE("no JavaVM"); return nullptr; }
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) { LOGE("attach failed"); return nullptr; }

    // FindClass on a ptrace-attached native thread uses the SYSTEM classloader,
    // which cannot see com.android.systemui.* . Get SystemUI's own classloader
    // via ActivityThread.currentApplication().getClassLoader() and loadClass()
    // the app classes through it.
    jobject gClassLoader = nullptr;
    jobject gApp = nullptr;                      // the SystemUI Application (a Context)
    jmethodID mLoadClass = nullptr;
    for (int i = 0; i < 600 && !gClassLoader; ++i) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        jclass cAT = env->FindClass("android/app/ActivityThread");            // boot classpath
        if (!cAT) { env->ExceptionClear(); usleep(500*1000); continue; }
        jmethodID mCurApp = env->GetStaticMethodID(cAT, "currentApplication", "()Landroid/app/Application;");
        if (!mCurApp) { env->ExceptionClear(); usleep(500*1000); continue; }
        jobject app = env->CallStaticObjectMethod(cAT, mCurApp);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!app) { usleep(500*1000); continue; }
        if (!gApp) gApp = env->NewGlobalRef(app);
        jclass cApp = env->GetObjectClass(app);
        jmethodID mGetCL = env->GetMethodID(cApp, "getClassLoader", "()Ljava/lang/ClassLoader;");
        if (!mGetCL) { env->ExceptionClear(); usleep(500*1000); continue; }
        jobject cl = env->CallObjectMethod(app, mGetCL);
        if (!cl) { usleep(500*1000); continue; }
        jclass cCL = env->GetObjectClass(cl);
        mLoadClass = env->GetMethodID(cCL, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (!mLoadClass) { env->ExceptionClear(); usleep(500*1000); continue; }
        gClassLoader = env->NewGlobalRef(cl);
        LOGI("got SystemUI classloader");
    }
    if (!gClassLoader || !mLoadClass) { LOGE("no app classloader"); return nullptr; }

    auto loadClass = [&](const char* name) -> jclass {
        jstring s = env->NewStringUTF(name);
        jobject c = env->CallObjectMethod(gClassLoader, mLoadClass, s);
        env->DeleteLocalRef(s);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        return (jclass)c;
    };

    // Resolve the reflection chain, retrying until SystemUI has built the objects.
    jobject gMech = nullptr;
    jmethodID mOnFpTouch = nullptr;
    for (int tries = 0; tries < 600 && !gMech; ++tries) {   // up to ~5 min
        if (env->ExceptionCheck()) env->ExceptionClear();
        jclass cDep = loadClass("com.android.systemui.DependencyEx");
        if (!cDep) { usleep(500*1000); continue; }
        jfieldID fSDep = env->GetStaticFieldID(cDep, "sDependency", "Lcom/android/systemui/DependencyEx;");
        if (!fSDep) { env->ExceptionClear(); usleep(500*1000); continue; }
        jobject oDep = env->GetStaticObjectField(cDep, fSDep);
        if (!oDep) { usleep(500*1000); continue; }
        jmethodID mGetDep = env->GetMethodID(cDep, "getDependency", "(Ljava/lang/Class;)Ljava/lang/Object;");
        jclass cOkde = loadClass("com.android.keyguard.OplusKeyguardDependencyEx");
        if (!mGetDep || !cOkde) { env->ExceptionClear(); usleep(500*1000); continue; }
        jobject oOkde = env->CallObjectMethod(oDep, mGetDep, cOkde);
        if (env->ExceptionCheck()) { env->ExceptionClear(); usleep(500*1000); continue; }
        if (!oOkde) { usleep(500*1000); continue; }
        jclass cOkdeR = env->GetObjectClass(oOkde);
        jmethodID mGetCtrl = env->GetMethodID(cOkdeR, "getOplusBiometricAuthController",
                                "()Lcom/android/systemui/biometrics/OplusBiometricAuthControllerEx;");
        if (!mGetCtrl) { env->ExceptionClear(); usleep(500*1000); continue; }
        jobject oCtrl = env->CallObjectMethod(oOkde, mGetCtrl);
        if (env->ExceptionCheck()) { env->ExceptionClear(); usleep(500*1000); continue; }
        if (!oCtrl) { usleep(500*1000); continue; }
        jclass cCtrl = env->GetObjectClass(oCtrl);   // com.oplus.systemui.biometrics.OplusBiometricAuthController
        jfieldID fMech = env->GetFieldID(cCtrl, "onScreenFingerprintUiMech",
                                "Lcom/oplus/systemui/biometrics/finger/udfps/OnScreenFingerprintUiMech;");
        if (!fMech) { env->ExceptionClear(); usleep(500*1000); continue; }
        jobject oMech = env->GetObjectField(oCtrl, fMech);
        if (!oMech) { usleep(500*1000); continue; }   // mech not built until FOD feature inits
        jclass cMech = env->GetObjectClass(oMech);
        mOnFpTouch = env->GetMethodID(cMech, "onFpTouch", "(Z)V");
        if (!mOnFpTouch) { env->ExceptionClear(); usleep(500*1000); continue; }
        gMech = env->NewGlobalRef(oMech);
        LOGI("resolved OnScreenFingerprintUiMech + onFpTouch");
    }
    if (!gMech || !mOnFpTouch) { LOGE("could not resolve mech"); return nullptr; }

    // Resolve QuickLaunchManager so we can wire up Quick Launch on the lock screen.
    //
    // On this OnePlus port, QuickLaunchManager.notifyFpIconAppear() has NO caller
    // (its natural trigger isn't wired), so the QuickLaunch container + touch
    // callback are never built when arriving at the lock screen — only after a
    // successful unlock. Without the callback, the gesture monitor receives the
    // finger but does nothing with it. We drive it ourselves:
    //   notifyFpIconAppear()  -> initQuickLaunchWindowIfNeed (builds container,
    //                            registers the touch callback, registers monitor)
    //   registerInputEvent()  -> ensures the "fp-quick-launch" gesture monitor is
    //                            live (idempotent: registerKgdInputReceiver early-
    //                            returns if already registered — it never disposes).
    // We NEVER call unregister: disposing the spy window is exactly what killed the
    // input channel before ("Touched window was removed" / "no input channel").
    // The other half of the bug: QuickLaunchContainer.touchUp sticks true after the
    // first gesture (doDispatchUpEvent sets it, and only notifyFpTouchDown() clears
    // it — which also has no caller on this port). While touchUp is true, onTouchDown
    // early-returns and never sets touchDownOrMove, so notifyFingerprintAuthSuccess
    // sees isTouchDown=false and Quick Launch never fires again ("works once, then
    // never"). We call notifyFpTouchDown() continuously while the finger is up to keep
    // touchUp cleared before the next press.
    jobject gQlm = nullptr; jmethodID mQlIconAppear = nullptr, mQlReg = nullptr, mQlTouchReset = nullptr;
    {
        jclass cQlm = loadClass("com.oplus.systemui.biometrics.finger.udfps.fpquicklaunch.quicklaunch.QuickLaunchManager");
        if (cQlm && gApp) {
            jmethodID mGet = env->GetStaticMethodID(cQlm, "getInstance",
                "(Landroid/content/Context;)Lcom/oplus/systemui/biometrics/finger/udfps/fpquicklaunch/quicklaunch/QuickLaunchManager;");
            if (mGet) {
                jobject qlm = env->CallStaticObjectMethod(cQlm, mGet, gApp);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (qlm) {
                    mQlIconAppear = env->GetMethodID(cQlm, "notifyFpIconAppear", "()V");
                    mQlReg = env->GetMethodID(cQlm, "registerInputEvent", "()V");
                    mQlTouchReset = env->GetMethodID(cQlm, "notifyFpTouchDown", "()V");
                    if (mQlIconAppear && mQlReg && mQlTouchReset) { gQlm = env->NewGlobalRef(qlm); LOGI("resolved QuickLaunchManager"); }
                    else env->ExceptionClear();
                }
            } else env->ExceptionClear();
        } else if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Publish resolved handles for the event-driven threads, then run fully
    // event-driven. The old 20ms poll + 2s JNI upkeep burned ~12% of a core inside
    // SystemUI all night; instead we block on __system_property_wait (0% CPU idle)
    // and act only on real events: FOD press (this thread) and screen-on (screenWorker).
    s_vm = vm;   s_mech = gMech;   s_onFpTouch = mOnFpTouch;
    s_qlm = gQlm; s_qlIcon = mQlIconAppear; s_qlReg = mQlReg; s_qlReset = mQlTouchReset;
    pthread_t stid; pthread_create(&stid, nullptr, screenWorker, nullptr); pthread_detach(stid);

    const prop_info* pi = __system_property_find("sys.bomb.fod");
    uint32_t serial = 0;   // 0 => first wait returns at once; then we track the real serial
    int last = -1;
    while (true) {
        if (!pi) { usleep(500 * 1000); pi = __system_property_find("sys.bomb.fod");
                   serial = 0; continue; }
        uint32_t ns = serial;
        __system_property_wait(pi, serial, &ns, nullptr);   // block until finger down/up
        serial = ns;
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("sys.bomb.fod", v);
        int state = (v[0] == '1') ? 1 : 0;
        if (state != last && bombEnabled()) {   // kill switch: no pressed-anim when off
            last = state;
            env->CallVoidMethod(gMech, mOnFpTouch, (jboolean)(state == 1));
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGI("onFpTouch(%d)", state);
            // On finger up, clear the sticky touchUp and re-prep for the next press.
            if (state == 0) qlSetup(env);
        }
    }
    return nullptr;
}

__attribute__((constructor))
static void on_load() {
    LOGI("libfodhook injected");
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    pthread_detach(t);
}
