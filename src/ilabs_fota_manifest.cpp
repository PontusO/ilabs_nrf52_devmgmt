// ilabs_fota_manifest.cpp -- tiny tolerant parser for the update manifest.
//
// Single-pass, allocation-free field extraction in the same spirit as the
// log-upload response parser. Not a general JSON parser: it locates known
// keys and reads the value that follows. Field order, surrounding
// whitespace, and unknown extra fields don't matter.

#include "ilabs_fota_manifest.h"
#include <string.h>

// Locate the key token `"key"` in [buf,end), then step past the following
// ':' and any surrounding whitespace. Returns a pointer to the first byte
// of the value, or NULL if the key is absent. The exact-quote match means
// "version" does not spuriously match inside "version_str".
static const char* find_value(const char* buf, const char* end,
                              const char* key) {
    size_t klen = strlen(key);
    for (const char* p = buf; p + klen + 2 <= end; p++) {
        if (p[0] == '"' && memcmp(p + 1, key, klen) == 0 &&
            p[1 + klen] == '"') {
            const char* q = p + klen + 2;   // just past the closing quote
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' ||
                               *q == '\n' || *q == ':')) {
                q++;
            }
            return q < end ? q : NULL;
        }
    }
    return NULL;
}

// Parse an unsigned integer at p: decimal, or hex when 0x/0X-prefixed.
// Stops at the first non-digit. Sets *ok false if no digits were read.
static uint32_t parse_uint(const char* p, const char* end, bool* ok) {
    *ok = false;
    if (p >= end) return 0;

    if (p + 2 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        uint32_t v = 0;
        bool any = false;
        while (p < end) {
            char c = *p;
            uint32_t d;
            if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else break;
            v = (v << 4) | d;
            p++; any = true;
        }
        *ok = any;
        return v;
    }

    uint32_t v = 0;
    bool any = false;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++; any = true;
    }
    *ok = any;
    return v;
}

// Copy a JSON string value (p points at the opening quote) into dst,
// NUL-terminated and bounded by dstsz. Returns 0 on success, -1 if p is
// not a quoted string or the string is unterminated. Backslash escapes are
// not interpreted (URLs and version strings don't need them) -- a
// backslash is copied literally.
static int copy_string(const char* p, const char* end,
                       char* dst, size_t dstsz) {
    if (p >= end || *p != '"' || dstsz == 0) return -1;
    p++;
    size_t i = 0;
    while (p < end && *p != '"') {
        if (i + 1 < dstsz) dst[i++] = *p;
        p++;
    }
    if (p >= end) return -1;   // unterminated quote
    dst[i] = '\0';
    return 0;
}

int ilabs_fota_manifest_parse(const char* buf, size_t len,
                              ilabs_fota_manifest_t* out) {
    if (!buf || !out) return -1;
    memset(out, 0, sizeof(*out));
    const char* end = buf + len;
    bool ok = false;

    // version (required)
    const char* v = find_value(buf, end, "version");
    if (!v) return -1;
    out->version = parse_uint(v, end, &ok);
    if (!ok) return -1;

    // device_type (required)
    const char* d = find_value(buf, end, "device_type");
    if (!d) return -1;
    out->device_type = parse_uint(d, end, &ok);
    if (!ok) return -1;

    // url (required)
    const char* u = find_value(buf, end, "url");
    if (!u || copy_string(u, end, out->url, sizeof(out->url)) != 0) return -1;
    if (out->url[0] == '\0') return -1;

    // version_str (optional, cosmetic)
    const char* s = find_value(buf, end, "version_str");
    if (s) copy_string(s, end, out->version_str, sizeof(out->version_str));

    return 0;
}
