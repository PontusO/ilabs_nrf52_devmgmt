// iLabs LTE FOTA -- streaming SHA-256.
//
// Self-contained SHA-256 implementation following NIST FIPS 180-4, with
// an API surface that matches mbedTLS's mbedtls_sha256_* naming. This
// means the implementation file can be swapped for upstream mbedTLS
// drop-in if a project ends up wanting the full library for other
// crypto needs -- all call sites stay the same.
//
// Usage:
//
//   ilabs_sha256_ctx ctx;
//   ilabs_sha256_init(&ctx);
//   ilabs_sha256_starts(&ctx);
//   while (have_more_chunks) {
//       ilabs_sha256_update(&ctx, chunk_buf, chunk_len);
//   }
//   uint8_t digest[32];
//   ilabs_sha256_finish(&ctx, digest);
//
// Library extraction note: this file moves verbatim to
// `src/ilabs_fota_sha256.h`. The implementation has no platform-
// specific code -- portable C99/C++11.

#ifndef ILABS_FOTA_SHA256_H
#define ILABS_FOTA_SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ILABS_SHA256_DIGEST_SIZE  32      // bytes
#define ILABS_SHA256_BLOCK_SIZE   64      // bytes

typedef struct {
    uint64_t total_len;                     // total bytes processed
    uint32_t state[8];                      // 8 working words (H0..H7)
    uint8_t  buffer[ILABS_SHA256_BLOCK_SIZE];
    size_t   buffer_len;                    // bytes currently in `buffer`
} ilabs_sha256_ctx;

// Initialise the context structure. Pairs with ilabs_sha256_free()
// (which currently does nothing but is provided for API symmetry with
// mbedTLS -- if we add CC310 hardware acceleration later, free will
// release the hardware context).
void ilabs_sha256_init(ilabs_sha256_ctx* ctx);
void ilabs_sha256_free(ilabs_sha256_ctx* ctx);

// Begin a new hash computation. May be called multiple times on the
// same context to reuse it.
void ilabs_sha256_starts(ilabs_sha256_ctx* ctx);

// Feed bytes into the hash. May be called any number of times with any
// chunk sizes including zero-length chunks (which is a no-op).
void ilabs_sha256_update(ilabs_sha256_ctx* ctx,
                           const uint8_t* data, size_t len);

// Finalise. Writes 32 bytes to `digest`. After this, the context must
// be re-started via ilabs_sha256_starts() before any further updates.
void ilabs_sha256_finish(ilabs_sha256_ctx* ctx,
                           uint8_t digest[ILABS_SHA256_DIGEST_SIZE]);

// One-shot convenience for whole-buffer hashing. Internally uses
// init/starts/update/finish. Use for small buffers; for streaming
// downloads call the chunked API directly.
void ilabs_sha256(const uint8_t* data, size_t len,
                    uint8_t digest[ILABS_SHA256_DIGEST_SIZE]);

// Hex-encode a digest into a 65-byte (64 + NUL) char buffer.
// Convenient for logging.
void ilabs_sha256_hex(const uint8_t digest[ILABS_SHA256_DIGEST_SIZE],
                        char out[2 * ILABS_SHA256_DIGEST_SIZE + 1]);

#ifdef __cplusplus
}
#endif

#endif // ILABS_FOTA_SHA256_H
