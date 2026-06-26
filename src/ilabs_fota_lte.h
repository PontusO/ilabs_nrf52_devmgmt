// iLabs LTE FOTA -- top-level orchestrator.
//
// Ties together lte_https_socket (transport) + ilabs_fota_gunzip
// (inflate) + ilabs_fota_sha256 (integrity) + ilabs_fota_qspi
// (staging). Single entry point: download a gzip-compressed slot image
// from an HTTPS URL, streaming-inflate during receive, write the
// payload bytes into the QSPI download slot, verify the slot header
// (CRC-16-CCITT + magic + device_type + slot_type) mid-stream and
// the payload SHA-256 at end-of-stream, then atomically commit the
// header at slot offset 0.
//
// Wire format produced by the host-side signing tool (phase 1 unsigned):
//
//     gz_stream -> [ ilabs_fota_slot_header_t (84 B) ] [ payload (N B) ]
//
// On-QSPI layout in the download slot once committed:
//
//     [ slot header @ 0..83 ] [ payload @ 84..84+N-1 ]
//
// The header is written LAST so that incomplete or aborted downloads
// leave the slot's first sector in its erased (0xFF) state -- the
// bootloader's magic check fails and the slot is correctly ignored.
//
// This module DOES NOT YET:
//   - handle signed slots (phase 2; rejected with an explicit error)
//   - write the bootloader settings block (caller does that via
//     FotaQspi.triggerUpdate() after a successful return)
//   - request reboot (caller does that via NVIC_SystemReset())
//
// Library extraction note: this file moves verbatim to
// `src/ilabs_fota_lte.h` and the orchestrator's `start()` becomes
// the iLabs_LteFota library's primary public method.

#ifndef ILABS_FOTA_LTE_H
#define ILABS_FOTA_LTE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      http_status;             // HTTP response status (200 on success);
                                      // 0 if no status line was ever parsed
    bool     stream_complete;         // gzip stream ended cleanly (TINF_DONE)
    bool     header_valid;            // slot header parsed and passed all
                                      // mid-stream checks (magic, sizes,
                                      // device_type, slot_type, CRC-16)
    bool     payload_sha_match;       // computed payload SHA-256 == the value
                                      // claimed in header.payload_sha256
    bool     slot_committed;          // slot header written to QSPI offset 0;
                                      // the bootloader will now treat the slot
                                      // as a valid candidate when it next runs
    size_t   compressed_bytes;        // total bytes received over HTTPS
    size_t   uncompressed_bytes;      // total bytes flowing out of gunzip
                                      // (header + payload). Subtract
                                      // ILABS_SLOT_HEADER_SIZE for payload.
    uint8_t  compressed_sha256[32];   // SHA-256 over the compressed payload --
                                      // transport-integrity check, compare to
                                      // the hash supplied out-of-band (e.g.
                                      // LoRa downlink that triggered the OTA)
    uint8_t  payload_sha256[32];      // SHA-256 over the uncompressed payload
                                      // bytes written to the download slot.
                                      // Cross-checked against the claim in the
                                      // slot header before the commit step.
    uint32_t header_fw_version;       // fw_version field from the slot header;
                                      // 0 if header_valid is false
    int      uzlib_rc;                // last uzlib return code; 0/TINF_DONE on
                                      // success, negative TINF_* on error
} ilabs_fota_result_t;

// Run a one-shot OTA download.
//   url:    "https://host/path/file.slot.gz" -- gzip-wrapped slot image
//           (slot header followed by payload bytes)
//   result: out-only; populated with diagnostics regardless of success
// Returns true iff the slot header was committed -- meaning every step
// passed (HTTPS, gunzip, header validate, payload SHA match) and the
// bootloader will see a valid candidate slot when it next runs.
//
// On false return, inspect result->* to see how far we got. The
// download slot's first sector remains in its erased (0xFF) state any
// time we don't commit, so a failed attempt is not observable by the
// bootloader.
//
// Blocks the calling task for the duration of the download (typically
// 30-180 s on LTE-M). Internally erases download-slot sectors just-in-
// time as the gunzip stream produces output.
bool ilabs_fota_download(const char* url,
                           ilabs_fota_result_t* result);

#ifdef __cplusplus
}
#endif

#endif // ILABS_FOTA_LTE_H
