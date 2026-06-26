// iLabs LTE FOTA -- gzip decompression wrapper around vendored uzlib.
//
// Sits on top of pfalcon/uzlib (the .c files in this directory). Adds:
//   - extern "C" linkage glue so C++ callers can use uzlib without
//     name-mangling collisions
//   - ilabs-style one-shot API for compact callers + self-tests
//   - streaming API for the OTA orchestrator (input/output drained
//     incrementally)
//
// The sliding window dictionary buffer is caller-supplied so we don't
// hardcode RAM allocation. Use:
//   - 32 KB for full RFC 1951 deflate compatibility (any compressor)
//   - smaller (e.g. 1 KB) for tests where input data is known short
//
// Library extraction note: this file moves verbatim to
// `src/ilabs_fota_gunzip.h`. The uzlib *.c/*.h files become a
// subdirectory `src/uzlib/` of the library, and the include path in
// this header changes to `"uzlib/uzlib.h"`.

#ifndef ILABS_FOTA_GUNZIP_H
#define ILABS_FOTA_GUNZIP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring uzlib's C declarations into scope under C linkage.
#include "uzlib/uzlib.h"

// ---- One-shot gunzip ----

// Decompresses a complete gzip stream `src[0..src_len-1]` into `dst`.
// `dict` is the sliding window backing buffer (caller-owned), at least
// `dict_size` bytes; pass 32768 for full deflate compatibility.
// On success returns the produced byte count (0..dst_cap). On failure
// returns one of:
//   -1  parameter / header error
//   -2  inflate decoding error (corrupt stream)
//   -3  output buffer too small (dst_cap < uncompressed size)
//   -4  gzip footer (CRC32 / length) mismatch
// `ilabs_fota_gunzip_init()` must have been called at least once
// before this function -- typically once at boot.
int ilabs_fota_gunzip_oneshot(const uint8_t* src, size_t src_len,
                                uint8_t* dst, size_t dst_cap,
                                uint8_t* dict, size_t dict_size);

// Initialise uzlib's static tables. Idempotent. Cheap (~1 ms). Call
// once at boot before any other ilabs_fota_gunzip_* function.
void ilabs_fota_gunzip_init(void);

// ---- Streaming gunzip (used by the OTA orchestrator) ----
//
// The streaming API will be wired in alongside the LTE OTA orchestrator
// in a follow-up commit -- skeleton declared here for forward
// reference. Caller sets up a uzlib_uncomp context, feeds compressed
// bytes via source pointers, and drains the dest buffer when it fills.

#ifdef __cplusplus
}
#endif

#endif // ILABS_FOTA_GUNZIP_H
