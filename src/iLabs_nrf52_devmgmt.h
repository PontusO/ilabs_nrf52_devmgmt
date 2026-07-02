// iLabs nRF52 Device Management -- public umbrella header.
//
// One include for sketches: `#include <iLabs_nrf52_devmgmt.h>`. Exposes
// the iLabsDevMgmt singleton `DevMgmt` (legacy aliases `iLabsFotaClass` /
// `FOTA` are kept for source compatibility), which orchestrates an LTE
// (or any other) HTTPS firmware-over-the-air update for nRF52 devices
// with an external QSPI flash and an Adafruit-fork bootloader, plus a
// transport-agnostic log-upload path (uploadLog) and a pull-based
// update check (checkForUpdate).
//
// Architecture: the library owns the FOTA pipeline -- download a
// gzip'd "slot" image, stream-inflate it, stage it into the QSPI
// download partition, verify SHA-256 against the slot header, and arm
// the bootloader settings block. It does NOT own a modem driver: the
// byte transport is injected (setTransport / setTestTransport /
// setUploadTransport) and all project couplings (logging, sleep locks,
// LoRa disable) are injected hooks. See ilabs_fota_transport.h for the
// function-pointer types -- INCLUDING the POST delivery contract that
// transports must honour -- and the examples/ directory for an
// Adrastea-I wiring.
//
// Retry ownership map (who retries what -- keep it this way, stacking
// another retry layer multiplies attempts):
//   - THIS LIBRARY retries a log-upload POST (POST_MAX_ATTEMPTS) when the
//     transport reports "definitely not delivered" (status <= 0), and
//     walks the FOTA megachunk loop; it never retries a delivered
//     request.
//   - THE TRANSPORT owns resolving link-level ambiguity (e.g. waiting
//     for the modem's completion URC before declaring failure) and any
//     single-request pacing/timeouts. It does NOT re-send bodies.
//   - THE APPLICATION owns session-level retry (re-triggering a whole
//     upload/update on a later wake); it does not retry individual
//     requests.
//
// QSPI partition addresses are a fixed contract with the bootloader
// (ilabs_fota_settings.h / ilabs_fota_slot.h). They are compile-time
// constants, never runtime-settable -- a mismatch with the bootloader
// would brick the swap.

#ifndef ILABS_NRF52_DEVMGMT_H
#define ILABS_NRF52_DEVMGMT_H

#include <stdint.h>
#include <stddef.h>

#include "ilabs_fota_transport.h"   // pluggable transport + hook typedefs
#include "ilabs_fota_lte.h"         // ilabs_fota_result_t + orchestrator
#include "ilabs_fota_test.h"        // ilabs_fota_test_result_t + self-test
#include "ilabs_log_upload.h"       // ilabs_log_upload_result_t + uploader
#include "ilabs_fota_manifest.h"    // update-check manifest type + parser
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
using iLabsUpdateCheck     = ilabs_fota_update_check_t;

class iLabsDevMgmt {
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
    // URL of the static update-check manifest polled by checkForUpdate().
    void setManifestUrl(const char* url);
    // Override the device_type the slot header is validated against.
    // Defaults to ILABS_DEVICE_TYPE (0x0052).
    void setDeviceType(uint32_t dev_type);
    // Range width per HTTPS GET in the megachunk loop. Default 100 KiB.
    void setMegachunkSize(size_t bytes);

    // ---- transport injection (register before update/self-test) ----
    void setTransport(ilabs_fota_https_get_range_fn range_get);
    void setTestTransport(ilabs_fota_https_get_fn plain_get);
    // HTTPS POST transport for log upload (register before uploadLog).
    // `max_body_bytes` is the transport's per-request body cap; the log
    // uploader sizes every compressed gzip member to fit it (this is how
    // e.g. the Adrastea %HTTPSEND 750-byte effective limit propagates --
    // declare it HERE, in one place, instead of duplicating the constant
    // in the library). Values below the compressor's worst-case floor
    // (~600 B) are clamped up to it.
    void setUploadTransport(ilabs_https_post_fn post_fn,
                            size_t max_body_bytes);

    // ---- optional hooks ----
    void setLogSink(ilabs_fota_log_fn fn, void* user = nullptr);
    // Session hooks, fired ONLY by update() around the whole OTA (begin
    // before the first transport call, end after the last QSPI/commit
    // step, success or failure). They exist for sketches that want the
    // LIBRARY to drive session bring-up/teardown (radio arbitration,
    // sleep locks). A sketch that orchestrates sessions EXTERNALLY (owns
    // the modem task and calls update()/uploadLog() from inside its own
    // session -- the pingday model) simply leaves them unregistered;
    // uploadLog()/checkForUpdate() never fire them either way. Note this
    // is a different concept from ilabs_log_source_t's begin/end, which
    // freeze the LOG STORE snapshot, not the link session.
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

    // Poll the configured manifest URL (setManifestUrl) over the ranged
    // (setTransport) transport -- the same reliable path update() uses; a
    // small bounded range fetches the whole manifest -- and decide whether a
    // newer image is offered for this device. `current_fw_version` is the
    // running firmware version word
    // (the caller's APP_FW_VERSION). Fills `out` with the manifest details
    // + a diagnostic status; returns true ONLY when out.update_available
    // (manifest device_type matches and version > current). The caller
    // then runs update(out.url, ...). No QSPI write, no session hooks.
    bool checkForUpdate(uint32_t current_fw_version, iLabsUpdateCheck& out);

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
    bool readSettings(ilabs_fota_settings_t& out) const;

    // ---- escape hatch to the raw QSPI staging layer ----
    iLabsFotaQspi& qspi();
};

// Pre-instantiated singleton, iLabs convention (cf. Adafruit `flash`).
extern iLabsDevMgmt DevMgmt;

// ---- legacy aliases (pre-0.5.0 API) --------------------------------------
// The class began life as FOTA-only; it now also owns log upload and the
// update check, so the primary names are iLabsDevMgmt / DevMgmt. Existing
// sketches using iLabsFotaClass / FOTA keep compiling unchanged.
using iLabsFotaClass = iLabsDevMgmt;
extern iLabsDevMgmt& FOTA;

#endif // ILABS_NRF52_DEVMGMT_H
