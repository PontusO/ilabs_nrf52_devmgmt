// iLabs LTE FOTA -- gzip wrapper (implementation).
//
// See ilabs_fota_gunzip.h for the public API contract.

#include "ilabs_fota_gunzip.h"

#include <string.h>

void ilabs_fota_gunzip_init(void) {
    uzlib_init();
}

int ilabs_fota_gunzip_oneshot(const uint8_t* src, size_t src_len,
                                uint8_t* dst, size_t dst_cap,
                                uint8_t* dict, size_t dict_size) {
    if (!src || !dst || !dict) return -1;
    if (src_len == 0 || dst_cap == 0 || dict_size == 0) return -1;

    // gzip stream needs at least the 10-byte header + 2-byte deflate
    // empty block + 8-byte footer = 20 bytes. Anything shorter is
    // definitely malformed.
    if (src_len < 20) return -1;

    struct uzlib_uncomp d;
    memset(&d, 0, sizeof(d));

    // Input: a fixed buffer, no read-callback needed.
    d.source         = src;
    d.source_limit   = src + src_len;
    d.source_read_cb = NULL;

    // Output: caller-supplied buffer.
    d.dest_start = dst;
    d.dest       = dst;
    d.dest_limit = dst + dst_cap;

    uzlib_uncompress_init(&d, dict, (unsigned int)dict_size);

    int r = uzlib_gzip_parse_header(&d);
    if (r != TINF_OK) return -1;

    // Loop until DONE or an error. uzlib_uncompress_chksum() does the
    // gzip CRC32 + length check at the end; uzlib_uncompress() doesn't.
    while (1) {
        r = uzlib_uncompress_chksum(&d);
        if (r == TINF_DONE) break;
        if (r == TINF_OK) {
            // Buffer filled before stream finished -- dst_cap too small.
            if (d.dest == d.dest_limit) return -3;
            // Otherwise (rare): more input needed but cb is NULL -- bail.
            return -2;
        }
        if (r == TINF_CHKSUM_ERROR) return -4;
        return -2;
    }

    return (int)(d.dest - d.dest_start);
}
