//
//  VRODiagnostics.cpp
//  ViroRenderer
//

#include "VRODiagnostics.h"

#if VRO_PLATFORM_ANDROID

#include <sys/system_properties.h>
#include <cstring>

namespace VRODiagnostics {
    std::atomic<bool> sVerboseEnabled{false};

    void refresh() {
        char value[PROP_VALUE_MAX] = {0};
        // log.tag.ViroDiag follows Android's standard Log.isLoggable convention.
        // Any non-empty value other than "S" (silent) enables verbose output;
        // we treat V/D/I as on to match developer intuition.
        int len = __system_property_get("log.tag.ViroDiag", value);
        bool enabled = false;
        if (len > 0) {
            char c = value[0];
            enabled = (c == 'V' || c == 'D' || c == 'I' || c == 'v' || c == 'd' || c == 'i');
        }
        sVerboseEnabled.store(enabled, std::memory_order_relaxed);
    }
}

#endif
