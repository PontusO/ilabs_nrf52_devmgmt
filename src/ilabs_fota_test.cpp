// iLabs OTA transport-corruption test -- implementation.
// See ilabs_fota_test.h.

#include "ilabs_fota_test.h"
#include "ilabs_fota_log.h"

#include "ilabs_fota_internal.h"

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Hardcoded test URL. Pontus uploads the pattern file (see
// prov/gen_test_pattern.py) to this path before triggering 't'.
static const char* const s_test_url =
    "https://ilabs.se/files/test_pattern.bin";

const char* ilabs_fota_test_url(void) {
    return s_test_url;
}

// Per-run state -- single-instance, static so it survives across the
// chunk callbacks. The callback runs synchronously inside
// lte_httpsGetSocket, same task as the caller -- no concurrency.
typedef struct {
    size_t cumulative_bytes;   // total bytes verified across chunks
    size_t mismatch_count;     // total bytes that differed from expected
    size_t first_mismatch_off; // offset of the very first mismatch
    int    log_budget;         // remaining first-N mismatches we'll log

    // Snapshot of the chunk where the first mismatch occurred. Kept
    // small to bound the log volume. Captured once, printed after the
    // download settles. Holds 8 expected/actual byte pairs centered
    // (roughly) on the first mismatch so the corruption pattern is
    // visible at a glance.
    bool   first_window_captured;
    size_t first_window_base;        // offset of byte 0 of the snapshot
    uint8_t first_window_exp[16];
    uint8_t first_window_got[16];
} test_state_t;

static test_state_t s_state;

static bool test_chunk_cb(const uint8_t* chunk, size_t chunk_len,
                          size_t total_received,
                          size_t content_length,
                          void* user) {
    (void)total_received; (void)content_length; (void)user;

    // Log every cb invocation with its length and the cumulative
    // count it's entering with. Helps reconcile receive-loop's view
    // vs. SOCKETDATA URC count -- if we see two cbs for the same
    // chunk index, or chunk_len < 1500 anywhere mid-stream, it
    // surfaces the discrepancy that's been baffling the analysis.
    LOG_DEBUG("[test] cb chunk_len=%lu cumulative_before=%lu",
              (unsigned long)chunk_len,
              (unsigned long)s_state.cumulative_bytes);

    for (size_t i = 0; i < chunk_len; ++i) {
        const size_t off      = s_state.cumulative_bytes + i;
        const uint8_t expected = (uint8_t)(off & 0xFF);
        const uint8_t got      = chunk[i];

        if (got != expected) {
            if (s_state.mismatch_count == 0) {
                s_state.first_mismatch_off = off;
            }
            if (s_state.log_budget > 0) {
                LOG_WARN("[test] mismatch at %lu: got 0x%02X expected 0x%02X "
                         "(diff bits 0x%02X)",
                         (unsigned long)off, got, expected,
                         (uint8_t)(got ^ expected));
                s_state.log_budget--;
            }
            s_state.mismatch_count++;

            // Bail out as soon as we have enough diagnostic info:
            // the first 16 mismatches were logged individually AND the
            // surrounding window was snapshotted. Anything beyond that
            // is just confirmation, and the LTE plan has a data cap.
            // Returning false tells lte_httpsGetSocket to close the
            // socket and stop pulling bytes from the server.
            if (s_state.log_budget <= 0 && s_state.first_window_captured) {
                LOG_WARN("[test] enough mismatches captured -- "
                         "aborting download to save data quota");
                return false;
            }

            // Snapshot the surrounding window once, at the first error.
            if (!s_state.first_window_captured) {
                s_state.first_window_captured = true;
                const size_t back = (i >= 8) ? 8 : i;
                s_state.first_window_base = off - back;
                for (size_t j = 0; j < 16; ++j) {
                    const size_t idx = i + j - back;
                    if (idx < chunk_len) {
                        s_state.first_window_got[j] = chunk[idx];
                        s_state.first_window_exp[j] =
                            (uint8_t)((s_state.first_window_base + j) & 0xFF);
                    } else {
                        // Past the chunk -- mark as 0xCC (debug filler) so the
                        // dump shows which positions weren't part of this
                        // single chunk.
                        s_state.first_window_got[j] = 0xCC;
                        s_state.first_window_exp[j] = 0xCC;
                    }
                }
            }
        }
    }

    s_state.cumulative_bytes += chunk_len;
    return true;
}

void ilabs_fota_test_run(const char* url, ilabs_fota_test_result_t* result) {
    memset(&s_state, 0, sizeof(s_state));
    s_state.log_budget = 16;     // log first 16 mismatches individually

    LOG_INFO("[test] starting pattern verification download: %s", url);
    int status = ilabs_fota__plain_get(url, test_chunk_cb, nullptr);

    LOG_INFO("[test] HTTP status:       %d", status);
    LOG_INFO("[test] bytes received:    %lu",
             (unsigned long)s_state.cumulative_bytes);
    LOG_INFO("[test] mismatches:        %lu",
             (unsigned long)s_state.mismatch_count);

    if (s_state.mismatch_count > 0) {
        LOG_ERROR("[test] first mismatch at: offset %lu",
                  (unsigned long)s_state.first_mismatch_off);
        LOG_ERROR("[test] window expected: %02X %02X %02X %02X %02X %02X %02X %02X "
                  "%02X %02X %02X %02X %02X %02X %02X %02X",
                  s_state.first_window_exp[0],  s_state.first_window_exp[1],
                  s_state.first_window_exp[2],  s_state.first_window_exp[3],
                  s_state.first_window_exp[4],  s_state.first_window_exp[5],
                  s_state.first_window_exp[6],  s_state.first_window_exp[7],
                  s_state.first_window_exp[8],  s_state.first_window_exp[9],
                  s_state.first_window_exp[10], s_state.first_window_exp[11],
                  s_state.first_window_exp[12], s_state.first_window_exp[13],
                  s_state.first_window_exp[14], s_state.first_window_exp[15]);
        LOG_ERROR("[test] window got:      %02X %02X %02X %02X %02X %02X %02X %02X "
                  "%02X %02X %02X %02X %02X %02X %02X %02X",
                  s_state.first_window_got[0],  s_state.first_window_got[1],
                  s_state.first_window_got[2],  s_state.first_window_got[3],
                  s_state.first_window_got[4],  s_state.first_window_got[5],
                  s_state.first_window_got[6],  s_state.first_window_got[7],
                  s_state.first_window_got[8],  s_state.first_window_got[9],
                  s_state.first_window_got[10], s_state.first_window_got[11],
                  s_state.first_window_got[12], s_state.first_window_got[13],
                  s_state.first_window_got[14], s_state.first_window_got[15]);
    } else {
        LOG_INFO("[test] PASS -- received bytes match the expected pattern");
    }

    if (result) {
        result->http_status       = status;
        result->bytes_received    = s_state.cumulative_bytes;
        result->mismatch_count    = s_state.mismatch_count;
        result->first_mismatch_off = s_state.first_mismatch_off;
        result->pass = (s_state.cumulative_bytes > 0 &&
                        s_state.mismatch_count == 0);
    }
}
