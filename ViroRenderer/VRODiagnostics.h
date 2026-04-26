//
//  VRODiagnostics.h
//  ViroRenderer
//
//  Two-tier diagnostic logging for HyperOS / OEM-skin investigation.
//
//  Tier 2 (lifecycle-rare, always-on) — use VRO_DIAG(category, fmt, ...).
//  Cost: one __android_log_print per call. Safe at any rate <= a few hundred
//  per second. Visible in logcat under tag "Viro.<category>".
//
//  Tier 3 (high-frequency, opt-in) — use VRO_DIAG_V(category, fmt, ...).
//  Reads system property `log.tag.ViroDiag` at lifecycle boundaries; cached
//  in an atomic bool. Production cost: one atomic load per call site, no I/O.
//  Tester enables with: adb shell setprop log.tag.ViroDiag V
//

#pragma once

#include "VRODefines.h"

#if VRO_PLATFORM_ANDROID

#include <android/log.h>
#include <atomic>

namespace VRODiagnostics {
    /*
     Cached fast-path readback. Set at lifecycle boundaries from refresh().
     Production builds: unset sysprop -> stays false -> all VRO_DIAG_V no-ops.
     */
    extern std::atomic<bool> sVerboseEnabled;

    /*
     Re-read log.tag.ViroDiag from system properties. Call once per lifecycle
     transition (onActivityResumed, onActivityPaused) — never in a hot path.
     */
    void refresh();

    inline bool isVerboseEnabled() {
        return sVerboseEnabled.load(std::memory_order_relaxed);
    }
}

/*
 Tier 2: lifecycle-rare, always emitted. Tag prefix lets tester filter:
   adb logcat -s Viro.Lifecycle:I Viro.Session:I Viro.Anchor:I
 */
#define VRO_DIAG(category, fmt, ...) \
    __android_log_print(ANDROID_LOG_INFO, "Viro." category, fmt, ##__VA_ARGS__)

/*
 Tier 3: gated by sysprop. Compiles to a load + branch in production.
 Use for per-frame events, internal cleanup markers, anything that could fire
 thousands of times.
 */
#define VRO_DIAG_V(category, fmt, ...) \
    do { \
        if (VRODiagnostics::isVerboseEnabled()) { \
            __android_log_print(ANDROID_LOG_INFO, "Viro." category, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#else // !VRO_PLATFORM_ANDROID

#define VRO_DIAG(category, fmt, ...)   ((void)0)
#define VRO_DIAG_V(category, fmt, ...) ((void)0)

namespace VRODiagnostics {
    inline void refresh() {}
    inline bool isVerboseEnabled() { return false; }
}

#endif
