/*
 * iLabs log upload -- implementation. See ilabs_log_upload.h.
 *
 * Compression strategy:
 *
 *   uzlib's tdefl-style API is too big for nRF52840 RAM (a full miniz
 *   tdefl_compressor would be ~320 KB). Instead we use uzlib_compress()
 *   with a caller-provided hash table -- ~4 KB at hash_bits=10. uzlib
 *   emits raw DEFLATE bytes, so we wrap with the 10-byte gzip header +
 *   8-byte trailer ourselves and the server stores the bytes verbatim as
 *   a .gz file.
 *
 *   uzlib_compress's LZ77 hash holds pointers into the source buffer, so
 *   back-references can only point inside whatever single buffer was
 *   passed -- so each upload "chunk" is one src->read -> one
 *   uzlib_compress -> one gzip member -> one POST. If the log delta is
 *   bigger than CHUNK_RAW_MAX, run() loops doing multiple POSTs in
 *   series. Each one advances src->mark() so a power cut in the middle
 *   doesn't re-send already-confirmed bytes.
 *
 * Memory footprint per call (all heap, freed at end):
 *
 *   raw_buf       CHUNK_RAW_MAX  bytes  (24 KB)
 *   hash_table    1 << hash_bits * 4 B  ( 4 KB at hash_bits=10)
 *   out_buf       CHUNK_OUT_MAX  bytes  (32 KB, holds worst-case
 *                                         incompressible 24 KB + framing)
 *   ----                                    --------
 *   total                                   ~60 KB transient
 */

#include "ilabs_log_upload.h"
#include "ilabs_fota_internal.h"   // ilabs_log_upload__post bridge
#include "ilabs_fota_sha256.h"
#include "ilabs_fota_log.h"        // LOG_* -> registered sink

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "uzlib/uzlib.h"
}

// zlib_start_block / zlib_finish_block come from uzlib/defl_static.h
// (pulled in transitively by uzlib.h).

// ---- Tunables -----------------------------------------------------------

#define CHUNK_RAW_MAX   (24u * 1024u)   /* raw bytes per upload chunk */
#define CHUNK_OUT_MAX   (32u * 1024u)   /* compressed-output buffer cap */
#define HASH_BITS       10u             /* 1024 entries * 4 B = 4 KB */

// Maximum number of POSTs per call to ilabs_log_upload_run(). Bounded so
// a misbehaving server-side never burns the whole modem-on session.
#define MAX_CHUNKS_PER_CALL  10u

// ---- Gzip framing helpers -----------------------------------------------

static void writeGzipHeader(uint8_t* out) {
    out[0] = 0x1F;  out[1] = 0x8B;  // magic
    out[2] = 0x08;                  // CM = DEFLATE
    out[3] = 0x00;                  // FLG = 0 (no extras)
    out[4] = out[5] = out[6] = out[7] = 0x00;   // MTIME = 0
    out[8] = 0x00;                  // XFL = 0
    out[9] = 0xFF;                  // OS = unknown
}

static void writeGzipTrailer(uint8_t* out, uint32_t crc, uint32_t isize) {
    out[0] = (uint8_t)(crc       & 0xFF);
    out[1] = (uint8_t)((crc >> 8) & 0xFF);
    out[2] = (uint8_t)((crc >> 16) & 0xFF);
    out[3] = (uint8_t)((crc >> 24) & 0xFF);
    out[4] = (uint8_t)(isize       & 0xFF);
    out[5] = (uint8_t)((isize >> 8) & 0xFF);
    out[6] = (uint8_t)((isize >> 16) & 0xFF);
    out[7] = (uint8_t)((isize >> 24) & 0xFF);
}

// ---- Response parsing ---------------------------------------------------

// We're parsing a tiny JSON body of shape {"received": N, ...} from the
// PHP handler. Single-buffer parse done in the chunk callback.
struct ResponseCtx {
    char     buf[128];
    size_t   buf_len;
    uint32_t received;   // parsed value, UINT32_MAX if not found
};

static bool responseCb(const uint8_t* chunk, size_t chunk_len,
                       size_t total_received, size_t content_length,
                       void* user) {
    (void)total_received; (void)content_length;
    ResponseCtx* r = static_cast<ResponseCtx*>(user);
    // Accumulate up to buf size; ignore overflow (we only need the
    // "received" key which sits near the start).
    if (r->buf_len + chunk_len < sizeof(r->buf)) {
        memcpy(r->buf + r->buf_len, chunk, chunk_len);
        r->buf_len += chunk_len;
    }
    return true;
}

static uint32_t parseReceivedField(const char* body, size_t len) {
    // Tiny parser -- finds "received" then scans for the first ASCII
    // digit and reads decimal until non-digit.
    const char* p   = body;
    const char* end = body + len;
    const char  key[] = "\"received\"";
    while (p + sizeof(key) - 1 < end) {
        if (memcmp(p, key, sizeof(key) - 1) == 0) {
            p += sizeof(key) - 1;
            while (p < end && (*p == ' ' || *p == ':' || *p == '\t')) p++;
            uint32_t v = 0;
            bool any = false;
            while (p < end && *p >= '0' && *p <= '9') {
                v = v * 10u + (uint32_t)(*p - '0');
                p++; any = true;
            }
            return any ? v : UINT32_MAX;
        }
        p++;
    }
    return UINT32_MAX;
}

// ---- One upload chunk ---------------------------------------------------

static bool uploadOneChunk(const char* url, const ilabs_log_source_t* src,
                           uint32_t start_offset, uint32_t raw_len,
                           ilabs_log_upload_result_t* result) {
    // Allocate transient buffers from heap. Freed before return.
    uint8_t* raw_buf = (uint8_t*)malloc(raw_len);
    if (!raw_buf) {
        result->status = ILABS_LOG_UPLOAD_COMPRESS_FAIL;
        return false;
    }

    const size_t hash_count = (1u << HASH_BITS);
    uzlib_hash_entry_t* hash_table =
        (uzlib_hash_entry_t*)calloc(hash_count, sizeof(uzlib_hash_entry_t));
    if (!hash_table) {
        free(raw_buf);
        result->status = ILABS_LOG_UPLOAD_COMPRESS_FAIL;
        return false;
    }

    uint8_t* out_buf = (uint8_t*)malloc(CHUNK_OUT_MAX);
    if (!out_buf) {
        free(hash_table); free(raw_buf);
        result->status = ILABS_LOG_UPLOAD_COMPRESS_FAIL;
        return false;
    }

    // 1. Read the raw bytes out of the log.
    size_t n = raw_len;
    if (!src->read(start_offset, raw_buf, &n, src->user) || n != raw_len) {
        free(out_buf); free(hash_table); free(raw_buf);
        result->status = ILABS_LOG_UPLOAD_COMPRESS_FAIL;
        return false;
    }

    // 2. Gzip header at offset 0.
    writeGzipHeader(out_buf);

    // 3. uzlib_compress straight into out_buf, starting after the
    //    gzip header. Reserve 8 bytes at the end for the trailer.
    struct uzlib_comp comp;
    memset(&comp, 0, sizeof(comp));
    comp.dict_size  = 32768;
    comp.hash_bits  = HASH_BITS;
    comp.hash_table = hash_table;
    comp.outbuf     = out_buf;
    comp.outsize    = CHUNK_OUT_MAX - 8;   // leave room for trailer
    comp.outlen     = 10;                  // skip the gzip header
    comp.outbits    = 0;
    comp.noutbits   = 0;

    zlib_start_block(&comp);
    uzlib_compress(&comp, raw_buf, raw_len);
    zlib_finish_block(&comp);

    if (comp.outlen + 8 > CHUNK_OUT_MAX) {
        // Compressed bigger than worst-case budget -- shouldn't happen
        // with a 33% safety margin, but bail rather than over-write.
        free(out_buf); free(hash_table); free(raw_buf);
        result->status = ILABS_LOG_UPLOAD_COMPRESS_FAIL;
        return false;
    }

    // 4. Compute gzip CRC32 over the raw input, append trailer.
    uint32_t crc = uzlib_crc32(raw_buf, raw_len, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
    writeGzipTrailer(out_buf + comp.outlen, crc, raw_len);
    size_t out_len = comp.outlen + 8;

    free(raw_buf);
    raw_buf = nullptr;

    // 5. SHA-256 of the compressed bytes -> hex for the X-Log-SHA256
    //    header. Lets the server reject a corrupt POST (the Adrastea
    //    socket transport has a known mid-stream byte-drop hazard).
    uint8_t digest[ILABS_SHA256_DIGEST_SIZE];
    ilabs_sha256(out_buf, out_len, digest);
    char sha_hex[2 * ILABS_SHA256_DIGEST_SIZE + 1];
    ilabs_sha256_hex(digest, sha_hex);

    // 6. POST via the injected transport.
    ResponseCtx ctx = { {0}, 0, UINT32_MAX };
    int http_status = ilabs_log_upload__post(url, out_buf, out_len,
                                             sha_hex, responseCb, &ctx);

    free(out_buf);
    free(hash_table);

    result->raw_bytes        = raw_len;
    result->compressed_bytes = (uint32_t)out_len;
    result->http_status      = http_status;

    if (http_status < 0) {
        result->status = ILABS_LOG_UPLOAD_HTTP_FAIL;
        return false;
    }
    if (http_status < 200 || http_status >= 300) {
        result->status = ILABS_LOG_UPLOAD_SERVER_REJECT;
        return false;
    }

    uint32_t srv_received = parseReceivedField(ctx.buf, ctx.buf_len);
    result->server_received = srv_received;

    if (srv_received == UINT32_MAX || srv_received != out_len) {
        result->status = ILABS_LOG_UPLOAD_MISMATCH;
        return false;
    }

    // 7. Successful upload -- advance the high-water so the next call
    //    only sends what's accumulated since.
    src->mark(start_offset + raw_len, src->user);
    result->status = ILABS_LOG_UPLOAD_OK;
    return true;
}

// ---- Public entry -------------------------------------------------------

bool ilabs_log_upload_run(const char* url,
                          const ilabs_log_source_t* src,
                          ilabs_log_upload_result_t* result) {
    if (!url || !src || !result) return false;
    if (!src->read || !src->total || !src->uploaded || !src->mark) return false;
    memset(result, 0, sizeof(*result));

    uint32_t start = src->uploaded(src->user);
    uint32_t end   = src->total(src->user);
    if (end <= start) {
        result->status = ILABS_LOG_UPLOAD_NO_NEW_DATA;
        return true;
    }

    LOG_INFO("[log-up] %lu B pending (offset %lu -> %lu)",
             (unsigned long)(end - start),
             (unsigned long)start, (unsigned long)end);

    uint32_t total_raw        = 0;
    uint32_t total_compressed = 0;
    int      last_http_status = 0;
    uint32_t last_server_rx   = 0;

    // Snapshot the end offset ONCE. The upload itself emits log lines
    // (LOG_INFO "[log-up]...", etc.) which get persisted and would
    // otherwise keep extending total() -- a feedback loop where every
    // upload spawns an extra tiny chunk that never quite catches up.
    // Bounding to `end` means new log generated during this upload
    // simply waits for the next trigger.
    const uint32_t end_snapshot = end;

    for (uint32_t chunk_i = 0;
         chunk_i < MAX_CHUNKS_PER_CALL;
         chunk_i++) {
        uint32_t cur_start = src->uploaded(src->user);
        if (cur_start >= end_snapshot) break;

        uint32_t remaining = end_snapshot - cur_start;
        uint32_t take = remaining < CHUNK_RAW_MAX ? remaining : CHUNK_RAW_MAX;

        ilabs_log_upload_result_t one;
        memset(&one, 0, sizeof(one));
        if (!uploadOneChunk(url, src, cur_start, take, &one)) {
            *result = one;       // failure: caller sees the chunk's diag
            return false;
        }
        total_raw        += one.raw_bytes;
        total_compressed += one.compressed_bytes;
        last_http_status  = one.http_status;
        last_server_rx    = one.server_received;
    }

    result->status          = ILABS_LOG_UPLOAD_OK;
    result->raw_bytes        = total_raw;
    result->compressed_bytes = total_compressed;
    result->http_status      = last_http_status;
    result->server_received  = last_server_rx;

    // No success log here -- the caller logs the richer result line
    // (incl. HTTP status + server received). Logging here too just
    // double-prints the same event.
    return true;
}
