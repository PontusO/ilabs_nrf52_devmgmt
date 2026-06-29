/*
 * iLabs log upload -- transport-agnostic compress + HTTPS POST.
 *
 * Reads the new portion of a persistent log (src->uploaded() ->
 * src->total()), gzips it on-device via the library's uzlib, computes
 * SHA-256 over the compressed bytes, and POSTs it to a caller-supplied
 * URL via the injected POST transport (FOTA.setUploadTransport()). On
 * 200 OK + matching server-reported byte count it calls src->mark() so
 * the next call only sends what has accumulated since.
 *
 * The log store, the watermark persistence, and the URL (incl. any
 * device-id path) all live in the sketch -- this orchestrator reaches
 * them only through the injected ilabs_log_source_t + the full URL passed
 * in. Mirrors the FOTA download's transport-injection design so the same
 * code works over LTE today and WiFi later.
 *
 * Compression note: uzlib_compress's LZ77 hash holds pointers into the
 * source buffer, so back-references can only point inside a single
 * buffer. Each upload "chunk" is therefore one src->read -> one
 * uzlib_compress -> one gzip member -> one POST. A log delta larger than
 * CHUNK_RAW_MAX loops inside run() doing multiple POSTs in series, each
 * advancing src->mark() so a power cut mid-run doesn't re-send confirmed
 * bytes.
 */

#ifndef ILABS_LOG_UPLOAD_H
#define ILABS_LOG_UPLOAD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ilabs_fota_transport.h"   // ilabs_log_source_t

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ILABS_LOG_UPLOAD_OK            = 0,   /* 200 OK, server confirmed byte count */
    ILABS_LOG_UPLOAD_NO_NEW_DATA   = 1,   /* nothing new since last mark() */
    ILABS_LOG_UPLOAD_COMPRESS_FAIL = 2,   /* alloc / compress / read error */
    ILABS_LOG_UPLOAD_HTTP_FAIL     = 3,   /* transport layer (socket/TLS) errored */
    ILABS_LOG_UPLOAD_SERVER_REJECT = 4,   /* server returned non-2xx */
    ILABS_LOG_UPLOAD_MISMATCH      = 5,   /* server 200 but received != sent */
} ilabs_log_upload_status_t;

typedef struct {
    ilabs_log_upload_status_t status;
    uint32_t                  raw_bytes;        /* uncompressed log bytes considered */
    uint32_t                  compressed_bytes; /* gzipped body bytes sent */
    int                       http_status;      /* server status, 0 if never received */
    uint32_t                  server_received;  /* parsed from JSON {"received": N} */
} ilabs_log_upload_result_t;

/* Run one upload attempt against `url` (already complete, incl. any
 * device-id path component). `src` injects the log store + watermark.
 * Sends through the transport registered via FOTA.setUploadTransport().
 * Returns true if status == OK or NO_NEW_DATA. */
bool ilabs_log_upload_run(const char* url,
                          const ilabs_log_source_t* src,
                          ilabs_log_upload_result_t* result);

#ifdef __cplusplus
}
#endif

#endif /* ILABS_LOG_UPLOAD_H */
