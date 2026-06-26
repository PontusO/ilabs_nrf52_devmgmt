// iLabs nRF52 FOTA -- internal logging shim.
//
// The extracted sources used the project's LOG_DEBUG/INFO/WARN/ERROR
// macros (log_manager.h + debug.h). To keep those call sites byte-for-
// byte unchanged while severing the project dependency, this header
// re-provides the same macro names, routed to the library's own log
// sink (ilabs_fota__logf -> the ilabs_fota_log_fn the sketch registers
// via FOTA.setLogSink()). When no sink is registered, logging is a
// no-op. Internal only -- not part of the public API.

#ifndef ILABS_FOTA_LOG_H
#define ILABS_FOTA_LOG_H

#include "ilabs_fota_internal.h"

#define LOG_DEBUG(...) ilabs_fota__logf(0, __VA_ARGS__)
#define LOG_INFO(...)  ilabs_fota__logf(1, __VA_ARGS__)
#define LOG_WARN(...)  ilabs_fota__logf(2, __VA_ARGS__)
#define LOG_ERROR(...) ilabs_fota__logf(3, __VA_ARGS__)

#endif // ILABS_FOTA_LOG_H
