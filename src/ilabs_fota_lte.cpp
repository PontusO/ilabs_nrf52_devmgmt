// iLabs LTE FOTA -- top-level orchestrator (implementation).
//
// See ilabs_fota_lte.h for the public API contract.

#include "ilabs_fota_lte.h"
#include "ilabs_fota_log.h"

#include "ilabs_fota_qspi.h"
#include "ilabs_fota_sha256.h"
#include "ilabs_fota_gunzip.h"
#include "ilabs_fota_internal.h"
#include "ilabs_fota_slot.h"

#include <Arduino.h>
#include <string.h>

// Compressed-stream accumulation buffer size. Must be at least one
// SOCKET_CHUNK_MAX (1500 B) plus enough residual headroom for uzlib's
// in-flight backlog when it pauses between calls (a single deflate
// block header can reference up to ~30 B of lookahead, but in practice
// the residual is small). 4 KB is plenty and the cost is negligible.
#define OTA_COMP_ACC_SIZE  4096

// One-OTA-at-a-time state. Static to avoid the 32 KB sliding window
// landing on a task stack. The orchestrator is single-instance by
// design -- starting a second download while one is in flight is
// undefined.
typedef struct {
    ilabs_sha256_ctx     compressed_sha;
    ilabs_sha256_ctx     payload_sha;
    struct uzlib_uncomp    uz;
    struct uzlib_uncomp    uz_backup;        // pre-call snapshot for rollback;
                                              // see ota_chunk_cb for the
                                              // src-starvation atomicity dance
    uint8_t                dict[32 * 1024];  // 32 KB deflate sliding window
    uint8_t                out_buf[256];     // page-sized drain buffer
    uint8_t                header_buf[ILABS_SLOT_HEADER_SIZE]; // first 84 B
                                                                // off gunzip
    size_t                 header_bytes_seen;   // 0..ILABS_SLOT_HEADER_SIZE
    bool                   header_validated;    // header passed all checks
    uint32_t               parsed_payload_size; // header.payload_size, after
                                                 // validate
    uint32_t               parsed_fw_version;   // header.fw_version, after
                                                 // validate
    uint8_t                parsed_payload_sha[32]; // header.payload_sha256

    // Sliding accumulator for compressed bytes. uzlib reads from here,
    // NOT directly from the chunk delivered by the HTTPS layer, so we
    // can pause uzlib cleanly at the end of a chunk. The compact-then-
    // append pattern in ota_chunk_cb keeps residual unconsumed bytes
    // at the front of the buffer between chunks.
    uint8_t                comp_acc[OTA_COMP_ACC_SIZE];
    size_t                 comp_acc_end;     // valid bytes in comp_acc

    uint32_t               qspi_offset;      // next slot offset to write to
                                             // (starts at HEADER_SIZE so the
                                             // payload goes after the header
                                             // gap; sector 0 erased by JIT
                                             // but bytes 0..83 left as 0xFF
                                             // until commit)
    uint32_t               next_unerased;    // next slot offset known NOT yet
                                             // erased (= sector boundary just
                                             // past the last erased sector)
    size_t                 payload_bytes_written; // bytes actually programmed
                                                   // into the slot as payload

    bool                   header_parsed_gz; // gzip header consumed from src
    bool                   stream_done;      // TINF_DONE seen
    bool                   pipeline_error;   // any sticky error inside cb
    bool                   src_starved;      // set by src_starved_cb whenever
                                              // uzlib tries to read past
                                              // comp_acc_end; signals the
                                              // outer loop to roll back state
    int                    last_uz_rc;       // last uzlib return code

    // Per-megachunk tracking. The orchestrator wraps multiple
    // ilabs_fota__range_get() calls (closed-range, ~100 KiB each) -- uzlib +
    // SHA + slot-write state above is preserved across megachunks;
    // only this flag resets per call. session_content_length is
    // vestigial -- each megachunk's response Content-Length is the
    // slice size, not the total -- but is retained for diagnostic
    // dumps.
    bool                   session_first_chunk; // true until cb sees the
                                                 // first body byte of this
                                                 // megachunk
    size_t                 session_content_length; // diagnostic: latest
                                                    // megachunk's
                                                    // Content-Length
    ilabs_fota_result_t* result;           // borrowed pointer, lives in caller
} ota_state_t;

static ota_state_t s_ota;

// Source-exhausted callback. uzlib's contract: returning -1 here sets
// d->eof = true and uzlib begins returning phantom 0 bytes from the
// stream, corrupting the inflater's bit buffer and state machine.
// We avoid that by snapshotting uzlib's state BEFORE each call and
// rolling back whenever this callback has fired -- the rollback undoes
// the phantom consumption. See ota_chunk_cb for the dance.
static int src_starved_cb(struct uzlib_uncomp* d) {
    (void)d;
    s_ota.src_starved = true;
    return -1;
}

// Ensure the sectors covering slot_offset..slot_offset+len are erased.
// Tracks the high-water mark of erased space in s_ota.next_unerased so
// each sector is erased exactly once. NOR flash requires sector-erase
// before page-write; doing it lazily here means the upfront cost of an
// OTA drops from "erase the whole 1 MB slot" (~14 s on this part) to
// "erase the first 4 KB sector" (~40 ms), with the remaining erases
// amortised across the much-slower cellular download.
//
// Side benefit for the commit step: the very first JIT call erases
// sector 0 (offsets 0..4095), leaving bytes 0..83 as 0xFF while
// payload is written at 84..4095 and beyond. Programming the header
// over those 0xFF bytes at the end of the download is a legal NOR
// write (each header byte only flips 1->0 bits), so no further erase
// is needed for commit.
static bool ensure_erased(uint32_t slot_offset, size_t len) {
    const uint32_t end       = slot_offset + len;
    const uint32_t sector_sz = iLabsFotaQspi::kSectorSize;

    while (s_ota.next_unerased < end) {
        if (s_ota.next_unerased >= iLabsFotaQspi::kDownloadSlotSize) {
            return false;
        }
        if (!FotaQspi.eraseDownloadSector(s_ota.next_unerased)) {
            LOG_ERROR("[fota] sector erase failed at slot off 0x%lX",
                      (unsigned long)s_ota.next_unerased);
            return false;
        }
        s_ota.next_unerased += sector_sz;
    }
    return true;
}

// Validate the 84-byte slot header we've just finished buffering.
// Runs mid-stream so we can abort early on a malformed header rather
// than waste the cellular budget downloading the rest.
static bool validate_slot_header(void) {
    // Copy into a stack-aligned struct so the field accesses below are
    // strict-aliasing safe regardless of where s_ota.header_buf lands
    // in the enclosing ota_state_t layout.
    ilabs_fota_slot_header_t hdr_copy;
    memcpy(&hdr_copy, s_ota.header_buf, sizeof(hdr_copy));
    const ilabs_fota_slot_header_t* hdr = &hdr_copy;

    if (hdr->magic != ILABS_SLOT_MAGIC) {
        LOG_ERROR("[fota] slot header magic mismatch: 0x%08lX (want 0x%08lX)",
                  (unsigned long)hdr->magic,
                  (unsigned long)ILABS_SLOT_MAGIC);
        return false;
    }
    if (hdr->header_size != ILABS_SLOT_HEADER_SIZE) {
        LOG_ERROR("[fota] slot header_size=%u (want %u)",
                  hdr->header_size, ILABS_SLOT_HEADER_SIZE);
        return false;
    }
    if (hdr->header_version != ILABS_SLOT_HEADER_VERSION) {
        LOG_ERROR("[fota] slot header_version=%u (want %u)",
                  hdr->header_version, ILABS_SLOT_HEADER_VERSION);
        return false;
    }
    if (hdr->device_type != ilabs_fota__device_type()) {
        LOG_ERROR("[fota] slot device_type=0x%04lX (want 0x%04X)",
                  (unsigned long)hdr->device_type,
                  (unsigned)ilabs_fota__device_type());
        return false;
    }
    if (hdr->slot_type != ILABS_SLOT_TYPE_DOWNLOAD) {
        LOG_ERROR("[fota] slot_type=%lu (want %u DOWNLOAD)",
                  (unsigned long)hdr->slot_type,
                  ILABS_SLOT_TYPE_DOWNLOAD);
        return false;
    }
    uint16_t want_crc = iLabsFotaQspi::crc16_ccitt(
        s_ota.header_buf, ILABS_SLOT_HEADER_SIZE - 2);
    if (want_crc != hdr->header_crc16) {
        LOG_ERROR("[fota] slot header CRC-16 mismatch: got 0x%04X want 0x%04X",
                  hdr->header_crc16, want_crc);
        return false;
    }
    if (hdr->flags & ILABS_SLOT_FLAG_SIGNED) {
        LOG_ERROR("[fota] slot claims signed; phase 1 only handles unsigned");
        return false;
    }
    if (hdr->payload_size >
        iLabsFotaQspi::kDownloadSlotSize - ILABS_SLOT_HEADER_SIZE) {
        LOG_ERROR("[fota] header.payload_size=%lu exceeds slot capacity",
                  (unsigned long)hdr->payload_size);
        return false;
    }

    s_ota.header_validated    = true;
    s_ota.parsed_payload_size = hdr->payload_size;
    s_ota.parsed_fw_version   = hdr->fw_version;
    memcpy(s_ota.parsed_payload_sha, hdr->payload_sha256, 32);

    s_ota.result->header_valid      = true;
    s_ota.result->header_fw_version = hdr->fw_version;

    LOG_INFO("[fota] slot header OK: fw=0x%08lX payload=%lu B",
             (unsigned long)hdr->fw_version,
             (unsigned long)hdr->payload_size);
    return true;
}

// Drain the uzlib output buffer. The first ILABS_SLOT_HEADER_SIZE bytes
// of the stream are peeled into s_ota.header_buf (NOT written to QSPI
// yet); subsequent bytes are written as payload at slot offset
// HEADER_SIZE + payload_bytes_written. Resets uzlib dest pointers to
// the buffer start for the next inflate round.
static bool drain_output(void) {
    size_t produced = (size_t)(s_ota.uz.dest - s_ota.uz.dest_start);
    if (produced == 0) return true;

    const uint8_t* src  = s_ota.uz.dest_start;
    size_t         left = produced;

    // Peel header bytes off the front of the produced data.
    if (s_ota.header_bytes_seen < ILABS_SLOT_HEADER_SIZE) {
        size_t need = ILABS_SLOT_HEADER_SIZE - s_ota.header_bytes_seen;
        size_t take = (left < need) ? left : need;
        memcpy(&s_ota.header_buf[s_ota.header_bytes_seen], src, take);
        s_ota.header_bytes_seen += take;
        src  += take;
        left -= take;

        // Header just became complete -- validate before continuing.
        // A malformed header makes the rest of the download wasted
        // cellular time, so fail fast.
        if (s_ota.header_bytes_seen == ILABS_SLOT_HEADER_SIZE) {
            if (!validate_slot_header()) {
                s_ota.pipeline_error = true;
                return false;
            }
        }
    }

    if (left > 0) {
        // Bounds: never write past the usable slot capacity.
        if (s_ota.qspi_offset + left >
            iLabsFotaQspi::kDownloadSlotSize) {
            LOG_ERROR("[fota] download slot overflow at offset 0x%lX",
                      (unsigned long)s_ota.qspi_offset);
            return false;
        }
        // Bounds: never write more payload than the header claims.
        // (Only enforceable after the header has been validated.)
        if (s_ota.header_validated &&
            s_ota.payload_bytes_written + left > s_ota.parsed_payload_size) {
            LOG_ERROR("[fota] payload exceeds header.payload_size=%lu",
                      (unsigned long)s_ota.parsed_payload_size);
            return false;
        }
        // JIT-erase any sectors the upcoming write touches.
        if (!ensure_erased(s_ota.qspi_offset, left)) return false;

        if (!FotaQspi.writeDownload(s_ota.qspi_offset, src, left)) {
            LOG_ERROR("[fota] QSPI write failed at offset 0x%lX",
                      (unsigned long)s_ota.qspi_offset);
            return false;
        }
        ilabs_sha256_update(&s_ota.payload_sha, src, left);
        s_ota.qspi_offset           += left;
        s_ota.payload_bytes_written += left;
    }

    s_ota.result->uncompressed_bytes += produced;

    // Reset dest pointers for the next inflate round.
    s_ota.uz.dest       = s_ota.out_buf;
    s_ota.uz.dest_limit = s_ota.out_buf + sizeof(s_ota.out_buf);
    return true;
}

// HTTPS chunk callback. Runs synchronously inside lte_httpsGet
// each time a %HTTPREAD produces decoded body bytes. The dance:
//
//   1. SHA the chunk and update bookkeeping.
//   2. Compact comp_acc -- shift any unconsumed bytes to offset 0 so
//      the next append doesn't run off the end.
//   3. Append the new chunk bytes to comp_acc and point uzlib's source
//      at the buffer.
//   4. Loop calling uzlib. Before each call, snapshot uz into uz_backup
//      and clear src_starved. After the call:
//         - if src_starved fired, restore the snapshot (undoing any
//           phantom-zero consumption) and break the loop; the rollback
//           leaves uzlib in the exact state it had before the call,
//           ready to resume when more compressed bytes arrive.
//         - on TINF_DONE: drain output, mark stream done.
//         - on any other negative rc: report and fail.
//         - on TINF_OK without starvation: dest must be full -- drain
//           and continue.
static bool ota_chunk_cb(const uint8_t* chunk, size_t chunk_len,
                         size_t total_received,
                         size_t content_length,
                         void* user) {
    (void)content_length; (void)user;

    if (s_ota.pipeline_error) return false;

    // First chunk of a megachunk: record this response's Content-Length
    // (= slice size, e.g. 100 KiB) for diagnostic logging. Megachunks
    // don't track a "total" -- the outer loop terminates on either
    // uzlib TINF_DONE or short delivery, not byte counting.
    if (s_ota.session_first_chunk) {
        s_ota.session_first_chunk = false;
        if (content_length > 0) {
            s_ota.session_content_length = content_length;
        }
    }

    // Transport SHA over the compressed bytes. `compressed_bytes` is
    // cumulative across resume sessions, so increment instead of
    // assigning total_received (which is per-session).
    ilabs_sha256_update(&s_ota.compressed_sha, chunk, chunk_len);
    s_ota.result->compressed_bytes += chunk_len;
    (void)total_received;

    // Compact comp_acc: shift the unconsumed tail (uz.source ..
    // comp_acc + comp_acc_end) down to offset 0 so we have room to
    // append. This also re-anchors uz.source/source_limit if uzlib
    // was rolled back into the buffer last round.
    size_t consumed = 0;
    if (s_ota.uz.source != nullptr && s_ota.comp_acc_end > 0) {
        consumed = (size_t)(s_ota.uz.source - s_ota.comp_acc);
    }
    size_t residual = s_ota.comp_acc_end - consumed;
    if (consumed > 0 && residual > 0) {
        memmove(s_ota.comp_acc, s_ota.comp_acc + consumed, residual);
    }
    s_ota.comp_acc_end = residual;

    if (chunk_len > sizeof(s_ota.comp_acc) - s_ota.comp_acc_end) {
        LOG_ERROR("[fota] comp_acc would overflow: residual=%lu chunk=%lu",
                  (unsigned long)s_ota.comp_acc_end,
                  (unsigned long)chunk_len);
        s_ota.pipeline_error = true;
        return false;
    }
    memcpy(s_ota.comp_acc + s_ota.comp_acc_end, chunk, chunk_len);
    s_ota.comp_acc_end += chunk_len;

    s_ota.uz.source       = s_ota.comp_acc;
    s_ota.uz.source_limit = s_ota.comp_acc + s_ota.comp_acc_end;
    s_ota.uz.eof          = 0;

    // First chunk must include enough bytes for the gzip header
    // (10 fixed + optional FEXTRA/FNAME/FCOMMENT/FHCRC). Typical
    // HTTPS RECEIVE delivers >= 500 B so this fits in one round, but
    // we use the same snapshot-and-rollback discipline as the inflate
    // loop for robustness against pathological tiny first chunks.
    if (!s_ota.header_parsed_gz) {
        memcpy(&s_ota.uz_backup, &s_ota.uz, sizeof(s_ota.uz_backup));
        s_ota.src_starved = false;
        int hdr = uzlib_gzip_parse_header(&s_ota.uz);
        if (s_ota.src_starved) {
            // Rolled back -- don't record `hdr`, it's the corrupted-state
            // return code from the run that consumed phantom zeros.
            memcpy(&s_ota.uz, &s_ota.uz_backup, sizeof(s_ota.uz));
            return true;     // need more bytes; try again next chunk
        }
        s_ota.last_uz_rc = hdr;
        if (hdr != TINF_OK) {
            LOG_ERROR("[fota] gzip header parse failed rc=%d", hdr);
            s_ota.pipeline_error = true;
            return false;
        }
        s_ota.header_parsed_gz = true;
    }

    // Streaming inflate.
    for (;;) {
        memcpy(&s_ota.uz_backup, &s_ota.uz, sizeof(s_ota.uz_backup));
        s_ota.src_starved = false;

        int r = uzlib_uncompress_chksum(&s_ota.uz);

        if (s_ota.src_starved) {
            // uzlib ran off the end of comp_acc and consumed phantom
            // zeros. Restore the snapshot to undo that and break out;
            // the next chunk's append will extend the source and we'll
            // resume from exactly here. Do NOT record `r` -- it's a
            // post-corruption return code that would mask the real
            // result reported back to the caller.
            memcpy(&s_ota.uz, &s_ota.uz_backup, sizeof(s_ota.uz));
            break;
        }
        s_ota.last_uz_rc = r;

        if (r == TINF_DONE) {
            if (!drain_output()) {
                s_ota.pipeline_error = true;
                return false;
            }
            s_ota.stream_done = true;
            return true;
        }
        if (r != TINF_OK) {
            LOG_ERROR("[fota] inflate error rc=%d at compressed_off=%lu",
                      r, (unsigned long)total_received);
            s_ota.pipeline_error = true;
            return false;
        }

        // r == TINF_OK and !starved => dest filled. Drain and continue.
        if (s_ota.uz.dest >= s_ota.uz.dest_limit) {
            if (!drain_output()) {
                s_ota.pipeline_error = true;
                return false;
            }
            continue;
        }
        // Belt-and-braces: shouldn't happen, but break rather than spin.
        break;
    }
    return true;
}

bool ilabs_fota_download(const char* url,
                           ilabs_fota_result_t* result) {
    if (!url || !result) return false;
    memset(result, 0, sizeof(*result));

    // Reset orchestrator state. Payload writes start at slot offset
    // ILABS_SLOT_HEADER_SIZE; sector 0 will be JIT-erased on the first
    // write, leaving the header gap at 0..83 as 0xFF until commit.
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.result      = result;
    s_ota.qspi_offset = ILABS_SLOT_HEADER_SIZE;

    // SHAs: one over compressed bytes (transport integrity), one over
    // payload bytes (cross-check against the header's claim).
    ilabs_sha256_init(&s_ota.compressed_sha);
    ilabs_sha256_starts(&s_ota.compressed_sha);
    ilabs_sha256_init(&s_ota.payload_sha);
    ilabs_sha256_starts(&s_ota.payload_sha);

    // uzlib: static tables + per-stream context. source/source_limit
    // are seeded to comp_acc on the first chunk callback; here we just
    // leave them at NULL/0.
    ilabs_fota_gunzip_init();
    uzlib_uncompress_init(&s_ota.uz, s_ota.dict, sizeof(s_ota.dict));
    s_ota.uz.source_read_cb = src_starved_cb;
    s_ota.uz.source         = nullptr;
    s_ota.uz.source_limit   = nullptr;
    s_ota.uz.dest_start     = s_ota.out_buf;
    s_ota.uz.dest           = s_ota.out_buf;
    s_ota.uz.dest_limit     = s_ota.out_buf + sizeof(s_ota.out_buf);

    LOG_INFO("[fota] starting HTTPS pull: %s", url);

    // Outer megachunk loop. Each iteration issues a closed-range GET
    // for the next ~100 KiB slice. The HTTP transport (lte_httpsGet,
    // %HTTPCMD path) is corruption-free end-to-end -- this strategy
    // sidesteps the one.com Apache ~130 s send_timeout that bit the
    // old open-range retry approach. uzlib state is preserved across
    // sessions in s_ota; the cb sees one continuous byte stream.
    //
    // Termination (any one triggers exit):
    //   - s_ota.stream_done       -> uzlib hit TINF_DONE (primary)
    //   - delivered < requested   -> server hit EOF (secondary safety
    //                                for cases where uzlib hasn't
    //                                consumed the full gzip footer yet
    //                                but the file is over)
    //   - s_ota.pipeline_error    -> hard error in the cb (slot
    //                                overflow, JIT-erase fail, etc.)
    //   - http_status 4xx/5xx     -> server refused; not retrying
    const size_t MEGACHUNK_SIZE = ilabs_fota__megachunk_size();
    int    http_status   = 0;
    int    megachunk_idx = 0;
    while (!s_ota.stream_done && !s_ota.pipeline_error) {
        size_t off = result->compressed_bytes;
        size_t end = off + MEGACHUNK_SIZE - 1;

        LOG_INFO("[fota] megachunk %d: bytes=%lu-%lu",
                 megachunk_idx + 1,
                 (unsigned long)off, (unsigned long)end);

        size_t before = result->compressed_bytes;
        s_ota.session_first_chunk = true;
        http_status = ilabs_fota__range_get(url, off, end, ota_chunk_cb, nullptr);
        size_t delivered = result->compressed_bytes - before;

        if (s_ota.pipeline_error) break;
        if (s_ota.stream_done) {
            LOG_INFO("[fota] megachunk %d: uzlib TINF_DONE (delivered %lu B)",
                     megachunk_idx + 1, (unsigned long)delivered);
            break;
        }

        // Server refused outright -- not going to change its mind.
        if (http_status >= 400 && http_status < 600) {
            LOG_ERROR("[fota] HTTP %d -- aborting", http_status);
            break;
        }

        LOG_INFO("[fota] megachunk %d done: delivered %lu of %lu B",
                 megachunk_idx + 1,
                 (unsigned long)delivered,
                 (unsigned long)MEGACHUNK_SIZE);

        // Short delivery means the server hit EOF. If uzlib hasn't
        // also reported done, the gzip stream was truncated -- bail
        // so the SHA verify catches it as an integrity failure.
        if (delivered < MEGACHUNK_SIZE) {
            if (!s_ota.stream_done) {
                LOG_ERROR("[fota] short megachunk (%lu < %lu) "
                          "but uzlib not done -- truncated stream",
                          (unsigned long)delivered,
                          (unsigned long)MEGACHUNK_SIZE);
            }
            break;
        }

        // Zero delivery on a non-final megachunk -- session collapsed
        // without giving us anything. Brief backoff and retry once,
        // otherwise bail. We don't expect this from clean HTTPCMD but
        // belt-and-braces.
        if (delivered == 0) {
            LOG_ERROR("[fota] zero-delivery megachunk -- aborting");
            break;
        }

        megachunk_idx++;
    }

    result->http_status     = http_status;
    result->stream_complete = s_ota.stream_done;
    result->uzlib_rc        = s_ota.last_uz_rc;

    // Finalise SHAs regardless of success so the caller can log what
    // we received even when the download truncated.
    ilabs_sha256_finish(&s_ota.compressed_sha, result->compressed_sha256);
    ilabs_sha256_finish(&s_ota.payload_sha,    result->payload_sha256);

    if (s_ota.pipeline_error) {
        LOG_ERROR("[fota] pipeline error during download -- slot not committed");
        return false;
    }
    if (http_status != 200 && http_status != 206) {
        LOG_ERROR("[fota] final HTTPS status %d -- slot not committed",
                  http_status);
        return false;
    }
    if (!s_ota.stream_done) {
        LOG_ERROR("[fota] gzip stream incomplete after %d megachunk(s) -- slot not committed",
                  megachunk_idx);
        return false;
    }
    if (!s_ota.header_validated) {
        LOG_ERROR("[fota] no slot header parsed (stream too short?) -- slot not committed");
        return false;
    }
    if (s_ota.payload_bytes_written != s_ota.parsed_payload_size) {
        LOG_ERROR("[fota] payload size mismatch: received %lu, header claims %lu -- slot not committed",
                  (unsigned long)s_ota.payload_bytes_written,
                  (unsigned long)s_ota.parsed_payload_size);
        return false;
    }
    if (memcmp(result->payload_sha256, s_ota.parsed_payload_sha, 32) != 0) {
        LOG_ERROR("[fota] payload SHA-256 mismatch vs header claim -- slot not committed");
        return false;
    }
    result->payload_sha_match = true;

    // Atomic commit: write the buffered header to slot offset 0. The
    // first sector (covering offsets 0..4095) was erased by JIT on
    // the first payload write, so the 0..83 byte range is 0xFF and a
    // straight program is legal.
    if (!FotaQspi.writeDownload(0, s_ota.header_buf, ILABS_SLOT_HEADER_SIZE)) {
        LOG_ERROR("[fota] failed to program slot header -- slot not committed");
        return false;
    }
    result->slot_committed = true;

    LOG_INFO("[fota] slot committed: fw=0x%08lX payload=%lu B compressed=%lu B",
             (unsigned long)s_ota.parsed_fw_version,
             (unsigned long)s_ota.payload_bytes_written,
             (unsigned long)result->compressed_bytes);
    return true;
}
