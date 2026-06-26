// iLabs OTA transport-corruption test.
//
// Downloads a known-pattern file via lte_httpsGetSocket and verifies
// each received byte against an in-RAM-computable expected value
// (byte[N] = N & 0xFF). NOT a real FOTA -- no slot wrapping, no
// uzlib, no QSPI write. The purpose is to isolate the question
// "is the modem/UART/AT chain delivering bytes faithfully?" without
// the noise of uzlib state, gzip framing, or slot validation.
//
// Generate the test file with prov/gen_test_pattern.py and upload it
// to the same web server as firmware.slot.gz; the URL the device
// pulls from is hardcoded below.
//
// The result is both logged via the library log sink AND returned in an
// ilabs_fota_test_result_t: total bytes received, total byte mismatches,
// offset of the first mismatch, and PASS/FAIL.

#ifndef ILABS_FOTA_TEST_H
#define ILABS_FOTA_TEST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    http_status;        // HTTP status from the plain GET transport
    size_t bytes_received;     // total bytes verified across all chunks
    size_t mismatch_count;     // bytes that differed from the expected pattern
    size_t first_mismatch_off; // offset of the first mismatch (valid if count>0)
    bool   pass;               // true iff bytes_received>0 and mismatch_count==0
} ilabs_fota_test_result_t;

// Run a pattern-verification download against `url` using the registered
// plain-GET transport. `result` is out-only and may be NULL if the
// caller only cares about the logged output.
void ilabs_fota_test_run(const char* url, ilabs_fota_test_result_t* result);

// Default pattern-file URL baked into the library. Callers may pass any
// URL to ilabs_fota_test_run(); this is just a convenience default.
const char* ilabs_fota_test_url(void);

#ifdef __cplusplus
}
#endif

#endif // ILABS_FOTA_TEST_H
