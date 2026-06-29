// ilabs_fota_manifest.h -- update-check manifest: types + parser (public C).
//
// The library can poll a small static JSON "manifest" that advertises the
// latest firmware for a board. iLabsFotaClass::checkForUpdate() fetches it
// through the injected plain-GET transport and compares the advertised
// version against the running firmware. The manifest is a *pointer*, not
// an authority: at install time the slot header's fw_version / device_type
// / SHA-256 remain the gate (see ilabs_fota_slot.h). No signing in this
// phase -- integrity rests on the slot header SHA-256 as before.
//
// Expected document (field order / whitespace tolerant; extra fields
// ignored). Integers may be decimal or 0x-prefixed hex:
//
//   {
//     "version":     16777225,        // (maj<<24)|(min<<16)|(patch<<8)|rev
//     "version_str": "1.0.0.9",       // human-facing only (optional)
//     "device_type": 82,              // must match this device's type
//     "url":         "https://.../fw-1.0.0.9.slot.gz"
//   }

#ifndef ILABS_FOTA_MANIFEST_H
#define ILABS_FOTA_MANIFEST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef ILABS_FOTA_URL_MAX
#define ILABS_FOTA_URL_MAX 256
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Parsed manifest contents.
typedef struct {
    uint32_t version;                   // advertised firmware version word
    uint32_t device_type;               // advertised target device type
    char     version_str[16];           // human-facing; "" if absent
    char     url[ILABS_FOTA_URL_MAX];   // firmware slot URL
} ilabs_fota_manifest_t;

// Parse a manifest document. Returns 0 on success (the required fields --
// version, device_type, url -- are all present and well-formed), negative
// on a missing or malformed required field. version_str is optional and
// left "" when absent. Does no network I/O and allocates nothing.
int ilabs_fota_manifest_parse(const char* buf, size_t len,
                              ilabs_fota_manifest_t* out);

// Why an update check ended (diagnostics). The bool return of
// checkForUpdate() is the actionable signal; this explains it.
typedef enum {
    ILABS_FOTA_CHECK_UPDATE_AVAILABLE = 0, // newer, matching image advertised
    ILABS_FOTA_CHECK_UP_TO_DATE       = 1, // manifest version <= running
    ILABS_FOTA_CHECK_WRONG_DEVICE     = 2, // manifest device_type mismatch
    ILABS_FOTA_CHECK_HTTP_FAIL        = 3, // transport/HTTP error or no URL
    ILABS_FOTA_CHECK_PARSE_FAIL       = 4, // body wasn't a usable manifest
    ILABS_FOTA_CHECK_NO_TRANSPORT     = 5, // no plain-GET transport registered
} ilabs_fota_check_status_t;

// Result of iLabsFotaClass::checkForUpdate().
typedef struct {
    bool                      update_available;       // newer & matching board
    ilabs_fota_check_status_t status;                 // why (diagnostics)
    int                       http_status;            // GET status (0/neg = err)
    uint32_t                  version;                // manifest version (0 = none)
    uint32_t                  device_type;            // manifest device_type
    char                      version_str[16];        // manifest version_str
    char                      url[ILABS_FOTA_URL_MAX];// manifest url ("" = none)
} ilabs_fota_update_check_t;

#ifdef __cplusplus
}
#endif

#endif // ILABS_FOTA_MANIFEST_H
