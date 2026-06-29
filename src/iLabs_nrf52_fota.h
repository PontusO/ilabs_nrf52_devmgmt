// iLabs nRF52 FOTA -- public umbrella header.
//
// One include for sketches: `#include <iLabs_nrf52_fota.h>`. Exposes the
// iLabsFotaClass singleton `FOTA`, which orchestrates an LTE (or any
// other) HTTPS firmware-over-the-air update for nRF52 devices with an
// external QSPI flash and an Adafruit-fork bootloader.
//
// Architecture: the library owns the FOTA pipeline -- download a
// gzip'd "slot" image, stream-inflate it, stage it into the QSPI
// download partition, verify SHA-256 against the slot header, and arm
// the bootloader settings block. It does NOT own a modem driver: the
// byte transport is injected (setTransport / setTestTransport) and all
// project couplings (logging, sleep locks, LoRa disable) are injected
// hooks. See ilabs_fota_transport.h for the function-pointer types and
// the examples/ directory for an Adrastea-I wiring.
//
// QSPI partition addresses are a fixed contract with the bootloader
// (ilabs_fota_settings.h / ilabs_fota_slot.h). They are compile-time
// constants, never runtime-settable -- a mismatch with the bootloader
// would brick the swap.

#ifndef ILABS_NRF52_FOTA_H
#define ILABS_NRF52_FOTA_H

#include <stdint.h>
#include <stddef.h>

#include "ilabs_fota_transport.h"   // pluggable transport + hook typedefs
#include "ilabs_fota_lte.h"         // ilabs_fota_result_t + orchestrator
#include "ilabs_fota_test.h"        // ilabs_fota_test_result_t + self-test
#include "ilabs_log_upload.h"       // ilabs_log_upload_result_t + uploader
#include "ilabs_fota_slot.h"        // ILABS_DEVICE_TYPE, ILABS_FW_VERSION()
#include "ilabs_fota_settings.h"    // partition map + settings struct
#include "ilabs_fota_qspi.h"        // iLabsFotaQspi + the FotaQspi singleton
#include "ilabs_fota_gunzip.h"      // uzlib gunzip wrapper
#include "ilabs_fota_sha256.h"      // SHA-256 utility

// Public result aliases (the underlying structs are plain C, declared in
// the headers above so the orchestrator can stay C-style internally).
using iLabsFotaResult     = ilabs_fota_result_t;
using iLabsFotaTestResult = ilabs_fota_test_result_t;
using iLabsLogUploadResult = ilabs_log_upload_result_t;

class iLabsFotaClass {
public:
    // ---- lifecycle ----
    // Bring up the QSPI staging layer and uzlib tables. Returns true if
    // the flash chip was detected. Call once from setup(), after the
    // transport/hooks are registered.
    bool begin();
    void suspend();           // QSPI chip -> deep-power-down (~1 uA)
    void resume();            // wake QSPI before an update
    bool ready() const;       // QSPI detected and live

    // ---- configuration ----
    // Default firmware URL, used by update() when called without one.
    void setFirmwareUrl(const char* url);
    // Override the device_type the slot header is validated against.
    // Defaults to ILABS_DEVICE_TYPE (0x0052).
    void setDeviceType(uint32_t dev_type);
    // Range width per HTTPS GET in the megachunk loop. Default 100 KiB.
    void setMegachunkSize(size_t bytes);

    // ---- transport injection (register before update/self-test) ----
    void setTransport(ilabs_fota_https_get_range_fn range_get);
    void setTestTransport(ilabs_fota_https_get_fn plain_get);
    // HTTPS POST transport for log upload (register before uploadLog).
    void setUploadTransport(ilabs_https_post_fn post_fn);

    // ---- optional hooks ----
    void setLogSink(ilabs_fota_log_fn fn, void* user = nullptr);
    void onSessionBegin(ilabs_fota_session_fn fn, void* user = nullptr);
    void onSessionEnd(ilabs_fota_session_fn fn, void* user = nullptr);

    // ---- actions ----
    // Run a full OTA. `url` == nullptr uses the configured firmware URL.
    // Fills `out` regardless; returns true iff the slot was committed.
    bool update(const char* url, iLabsFotaResult& out);
    bool update(iLabsFotaResult& out);

    // Transport-only pattern check (no QSPI write, no uzlib). `url` ==
    // nullptr uses the library's default pattern URL. Returns out.pass.
    bool transportSelfTest(const char* url, iLabsFotaTestResult& out);

    // Compress + POST the new portion of a log to `url` (already
    // complete, incl. any device-id path). `src` injects the log store +
    // watermark; the POST goes through setUploadTransport(). Runs inside
    // the caller's existing modem session (no session hooks fired). Fills
    // `out`; returns true if anything was uploaded or there was no new
    // data.
    bool uploadLog(const char* url, const ilabs_log_source_t& src,
                   iLabsLogUploadResult& out);

    // ---- bootloader-settings passthroughs ----
    bool triggerUpdate(uint32_t download_fw_version);
    bool confirmBoot(uint32_t current_app_fw_version);
    bool readSettings(ilabs_fota_settings_t& out) const;

    // ---- escape hatch to the raw QSPI staging layer ----
    iLabsFotaQspi& qspi();
};

// Pre-instantiated singleton, iLabs convention (cf. Adafruit `flash`).
extern iLabsFotaClass FOTA;

#endif // ILABS_NRF52_FOTA_H
