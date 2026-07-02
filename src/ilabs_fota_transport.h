// iLabs nRF52 FOTA -- pluggable transport + hook typedefs (public).
//
// The library does NOT own a modem driver. It pulls firmware bytes
// through a caller-supplied HTTPS-GET function and reports progress /
// lifecycle through caller-supplied hooks. This header declares the
// function-pointer types the sketch implements; nothing here includes
// any modem or project header, so the library stays self-contained and
// modem-agnostic.
//
// The chunk-callback signature is deliberately identical to the
// transport layer the orchestrator was extracted from
// (lte_http_chunk_cb_t / lte_https_chunk_cb_t), so the sketch's adapter
// is a one-line forward to lte_httpsGet() / lte_httpsGetSocket().

#ifndef ILABS_FOTA_TRANSPORT_H
#define ILABS_FOTA_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Delivered for every decoded body chunk during a download. Return true
// to keep receiving, false to abort the transfer (the transport must
// then close the connection and stop). `total_received` is cumulative
// for the current GET; `content_length` is this response's
// Content-Length (0 if unknown). `user` is the opaque pointer the
// library passed to the transport.
typedef bool (*ilabs_fota_chunk_cb_t)(const uint8_t* chunk, size_t chunk_len,
                                      size_t total_received,
                                      size_t content_length,
                                      void* user);

// Ranged HTTPS GET -- used for the FOTA megachunk loop. Requests the
// closed byte range [range_offset .. range_end] inclusive (a Range:
// header). Must invoke `cb` synchronously for each decoded chunk.
// Returns the HTTP status (200 or 206 on success), 0 if no status line
// was seen, or a negative value on a transport-level error.
typedef int (*ilabs_fota_https_get_range_fn)(const char* url,
                                             size_t range_offset,
                                             size_t range_end,
                                             ilabs_fota_chunk_cb_t cb,
                                             void* user);

// Plain (non-ranged) HTTPS GET -- used by the transport self-test.
// Returns the HTTP status (negative on transport error).
typedef int (*ilabs_fota_https_get_fn)(const char* url,
                                       ilabs_fota_chunk_cb_t cb,
                                       void* user);

// Optional log sink. `level`: 0=DEBUG 1=INFO 2=WARN 3=ERROR. `msg` is a
// fully-formatted, NUL-terminated line (no trailing newline). Called
// from the task that runs the download -- keep it cheap and non-blocking.
typedef void (*ilabs_fota_log_fn)(int level, const char* msg, void* user);

// Optional session lifecycle hooks. begin fires before any transport
// work; end fires after the last QSPI / commit step, on success or
// failure. The sketch uses these to disable LoRa + acquire a sleep lock
// for the duration of the modem session.
typedef void (*ilabs_fota_session_fn)(void* user);

// ---- Log-upload transport + source (mirror of the download side) -------
//
// The log-upload feature compresses + POSTs accumulated log bytes. Like
// the download path it owns no modem driver and no log store: the byte
// transport (POST) and the log store (read + watermark) are both
// injected. The POST transport reuses ilabs_fota_chunk_cb_t for the
// response body, so a sketch adapter forwarding to an existing socket
// POST function is type-checked at the call site.

// Injected HTTPS POST. `sha256_hex` (may be NULL) is sent as an integrity
// header; `response_cb` (may be NULL) receives the response body chunks.
// Returns the HTTP status (>=200 on a real response), 0 if no status line
// was seen, or a negative value on a transport-level error.
//
// DELIVERY CONTRACT (load-bearing -- read before writing a transport):
// the library RETRIES the same body whenever this returns <= 0. Therefore
// a return <= 0 is a promise that the request was DEFINITELY NOT delivered
// to the server. If the underlying stack can accept-and-execute a request
// while still reporting a local error (the Adrastea AT%HTTPSEND does
// exactly this), the transport MUST resolve the ambiguity itself -- e.g.
// wait for the modem's completion URC as ground truth -- before returning
// <= 0. Returning <= 0 for a request that was actually delivered causes a
// duplicate POST of the same bytes (for log upload: a duplicated gzip
// member in the server archive). If a transport cannot guarantee this,
// it must either return the real status or accept that duplicates occur.
typedef int (*ilabs_https_post_fn)(const char* url,
                                   const uint8_t* body, size_t body_len,
                                   const char* sha256_hex,
                                   ilabs_fota_chunk_cb_t response_cb,
                                   void* user);

// Injected log source. The library reads raw bytes and advances the
// upload high-water through these; the actual store + persistence live in
// the sketch (log_manager). `read` fills `buf` with up to *len_inout
// bytes starting at `offset`, writes the count back to *len_inout, and
// returns false on error. `total` is the current byte count on disk;
// `uploaded` is the persisted high-water; `mark` advances it after a
// confirmed chunk (must be monotonic + power-loss safe in the impl).
typedef bool     (*ilabs_log_read_fn)(uint32_t offset, uint8_t* buf,
                                      size_t* len_inout, void* user);
typedef uint32_t (*ilabs_log_total_fn)(void* user);
typedef uint32_t (*ilabs_log_uploaded_fn)(void* user);
typedef void     (*ilabs_log_mark_fn)(uint32_t consumed, void* user);

typedef struct {
    ilabs_log_read_fn     read;
    ilabs_log_total_fn    total;
    ilabs_log_uploaded_fn uploaded;
    ilabs_log_mark_fn     mark;
    // Optional (may be NULL) snapshot lifecycle. `begin` fires once before
    // the first read; the store should freeze what's uploadable now (e.g.
    // rotate to a fresh file so live logging can't mutate or prune the
    // bytes being read) so `total`/`read` see a stable snapshot for the
    // whole run. `end` fires once after the last chunk (success OR
    // failure) to release the freeze. Stores that are naturally stable
    // during an upload can leave both NULL.
    ilabs_fota_session_fn begin;
    ilabs_fota_session_fn end;
    void*                 user;
} ilabs_log_source_t;

#endif // ILABS_FOTA_TRANSPORT_H
