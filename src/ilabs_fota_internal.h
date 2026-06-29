// iLabs nRF52 FOTA -- internal plumbing (NOT part of the public API).
//
// C-linkage bridges that let the extracted C-style orchestrator
// (ilabs_fota_lte.cpp) and the transport self-test (ilabs_fota_test.cpp)
// reach the registered transport + log sink that the C++ iLabsFotaClass
// owns. Implemented in iLabs_nrf52_devmgmt.cpp. Sketch code never includes
// this header -- it uses the iLabsFotaClass setters instead.

#ifndef ILABS_FOTA_INTERNAL_H
#define ILABS_FOTA_INTERNAL_H

#include "ilabs_fota_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

// Formatted log -- forwards to the registered ilabs_fota_log_fn (no-op
// if none registered). level: 0=DEBUG 1=INFO 2=WARN 3=ERROR.
void ilabs_fota__logf(int level, const char* fmt, ...);

// Invoke the registered ranged / plain GET transport. Returns the
// transport's HTTP status, or -1 if no transport has been registered.
int  ilabs_fota__range_get(const char* url, size_t range_offset,
                           size_t range_end,
                           ilabs_fota_chunk_cb_t cb, void* user);
int  ilabs_fota__plain_get(const char* url,
                           ilabs_fota_chunk_cb_t cb, void* user);

// Invoke the registered HTTPS POST transport (log upload). Returns the
// transport's HTTP status, or -1 if no upload transport has been
// registered.
int  ilabs_log_upload__post(const char* url,
                            const uint8_t* body, size_t body_len,
                            const char* sha256_hex,
                            ilabs_fota_chunk_cb_t response_cb, void* user);

// Runtime-configurable parameters owned by iLabsFotaClass, read by the
// orchestrator. Defaults: device_type = ILABS_DEVICE_TYPE, megachunk =
// 100 KiB.
uint32_t ilabs_fota__device_type(void);
size_t   ilabs_fota__megachunk_size(void);

#ifdef __cplusplus
}
#endif

#endif // ILABS_FOTA_INTERNAL_H
