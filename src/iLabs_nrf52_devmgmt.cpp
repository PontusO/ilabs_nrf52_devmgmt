// iLabs nRF52 Device Management -- iLabsFotaClass implementation + bridges.
//
// Holds the registered transport / log / session hooks and the runtime
// configuration as file-scope state, exposes them to the C-style
// orchestrator + self-test through the extern "C" ilabs_fota__* bridges
// declared in ilabs_fota_internal.h, and provides the C++ class methods
// the sketch uses.

#include "iLabs_nrf52_devmgmt.h"
#include "ilabs_fota_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------
// Registered hooks + runtime config (set via the class, read via the
// bridges below). Single-instance: the orchestrator is single-instance
// by design, so there is no per-call ownership to track.
// ---------------------------------------------------------------------
static ilabs_fota_https_get_range_fn s_range_fn  = nullptr;
static ilabs_fota_https_get_fn       s_plain_fn  = nullptr;
static ilabs_https_post_fn           s_post_fn   = nullptr;
static size_t                        s_post_max  = 0;   // per-request body cap

static ilabs_fota_log_fn             s_log_fn    = nullptr;
static void*                         s_log_user  = nullptr;

static ilabs_fota_session_fn         s_begin_fn  = nullptr;
static void*                         s_begin_user = nullptr;
static ilabs_fota_session_fn         s_end_fn    = nullptr;
static void*                         s_end_user  = nullptr;

static char     s_url[256]     = { 0 };
static char     s_manifest_url[ILABS_FOTA_URL_MAX] = { 0 };
static uint32_t s_device_type  = ILABS_DEVICE_TYPE;
static size_t   s_megachunk    = 100 * 1024;

// ---------------------------------------------------------------------
// Internal C-linkage bridges (declared in ilabs_fota_internal.h).
// ---------------------------------------------------------------------
extern "C" void ilabs_fota__logf(int level, const char* fmt, ...) {
    if (!s_log_fn) return;     // no sink registered -> drop silently
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    s_log_fn(level, buf, s_log_user);
}

extern "C" int ilabs_fota__range_get(const char* url, size_t range_offset,
                                     size_t range_end,
                                     ilabs_fota_chunk_cb_t cb, void* user) {
    if (!s_range_fn) return -1;
    return s_range_fn(url, range_offset, range_end, cb, user);
}

extern "C" int ilabs_fota__plain_get(const char* url,
                                     ilabs_fota_chunk_cb_t cb, void* user) {
    if (!s_plain_fn) return -1;
    return s_plain_fn(url, cb, user);
}

extern "C" size_t ilabs_log_upload__max_body(void) {
    return s_post_max;
}

extern "C" int ilabs_log_upload__post(const char* url,
                                      const uint8_t* body, size_t body_len,
                                      const char* sha256_hex,
                                      ilabs_fota_chunk_cb_t response_cb,
                                      void* user) {
    if (!s_post_fn) return -1;
    return s_post_fn(url, body, body_len, sha256_hex, response_cb, user);
}

extern "C" uint32_t ilabs_fota__device_type(void) { return s_device_type; }
extern "C" size_t   ilabs_fota__megachunk_size(void) { return s_megachunk; }

// ---------------------------------------------------------------------
// iLabsFotaClass methods.
// ---------------------------------------------------------------------
bool iLabsDevMgmt::begin() {
    bool ok = FotaQspi.begin();
    ilabs_fota_gunzip_init();
    return ok;
}

void iLabsDevMgmt::suspend()       { FotaQspi.suspend(); }
void iLabsDevMgmt::resume()        { FotaQspi.resume(); }
bool iLabsDevMgmt::ready() const   { return FotaQspi.ready(); }

void iLabsDevMgmt::setFirmwareUrl(const char* url) {
    if (!url) { s_url[0] = '\0'; return; }
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
}

void iLabsDevMgmt::setManifestUrl(const char* url) {
    if (!url) { s_manifest_url[0] = '\0'; return; }
    strncpy(s_manifest_url, url, sizeof(s_manifest_url) - 1);
    s_manifest_url[sizeof(s_manifest_url) - 1] = '\0';
}

void iLabsDevMgmt::setDeviceType(uint32_t dev_type) { s_device_type = dev_type; }

void iLabsDevMgmt::setMegachunkSize(size_t bytes) {
    if (bytes) s_megachunk = bytes;
}

void iLabsDevMgmt::setTransport(ilabs_fota_https_get_range_fn range_get) {
    s_range_fn = range_get;
}
void iLabsDevMgmt::setTestTransport(ilabs_fota_https_get_fn plain_get) {
    s_plain_fn = plain_get;
}
void iLabsDevMgmt::setUploadTransport(ilabs_https_post_fn post_fn,
                                       size_t max_body_bytes) {
    s_post_fn  = post_fn;
    s_post_max = max_body_bytes;
}

void iLabsDevMgmt::setLogSink(ilabs_fota_log_fn fn, void* user) {
    s_log_fn = fn; s_log_user = user;
}
void iLabsDevMgmt::onSessionBegin(ilabs_fota_session_fn fn, void* user) {
    s_begin_fn = fn; s_begin_user = user;
}
void iLabsDevMgmt::onSessionEnd(ilabs_fota_session_fn fn, void* user) {
    s_end_fn = fn; s_end_user = user;
}

bool iLabsDevMgmt::update(const char* url, iLabsFotaResult& out) {
    const char* u = url ? url : (s_url[0] ? s_url : nullptr);
    if (!u) {
        memset(&out, 0, sizeof(out));
        ilabs_fota__logf(3, "[fota] no URL configured -- nothing to do");
        return false;
    }
    if (s_begin_fn) s_begin_fn(s_begin_user);
    bool ok = ilabs_fota_download(u, &out);
    if (s_end_fn) s_end_fn(s_end_user);
    return ok;
}

bool iLabsDevMgmt::update(iLabsFotaResult& out) {
    return update(nullptr, out);
}

bool iLabsDevMgmt::transportSelfTest(const char* url, iLabsFotaTestResult& out) {
    const char* u = url ? url : ilabs_fota_test_url();
    if (s_begin_fn) s_begin_fn(s_begin_user);
    ilabs_fota_test_run(u, &out);
    if (s_end_fn) s_end_fn(s_end_user);
    return out.pass;
}

// Update-check manifest fetch. The manifest is a handful of small fields;
// 512 B comfortably exceeds a well-formed document. We retain only the
// first 512 B and keep draining the body (return true) so the transport
// closes cleanly even if the server sends a trailing newline / extra.
namespace {
struct ManifestFetchCtx {
    char   buf[512];
    size_t len;
};
}  // namespace

static bool manifestFetchCb(const uint8_t* chunk, size_t chunk_len,
                            size_t total_received, size_t content_length,
                            void* user) {
    (void)total_received; (void)content_length;
    ManifestFetchCtx* c = static_cast<ManifestFetchCtx*>(user);
    size_t room = sizeof(c->buf) - c->len;
    size_t take = chunk_len < room ? chunk_len : room;
    if (take) { memcpy(c->buf + c->len, chunk, take); c->len += take; }
    return true;
}

bool iLabsDevMgmt::checkForUpdate(uint32_t current_fw_version,
                                    iLabsUpdateCheck& out) {
    memset(&out, 0, sizeof(out));

    if (!s_range_fn) {
        out.status = ILABS_FOTA_CHECK_NO_TRANSPORT;
        ilabs_fota__logf(3, "[fota-chk] no ranged GET transport registered");
        return false;
    }
    if (!s_manifest_url[0]) {
        out.status = ILABS_FOTA_CHECK_HTTP_FAIL;
        ilabs_fota__logf(3, "[fota-chk] no manifest URL configured");
        return false;
    }

    ManifestFetchCtx ctx;
    ctx.len = 0;
    // Fetch via the ranged (HTTPCMD) transport -- the SAME reliable path the
    // firmware download uses -- not a plain socket GET. On Adrastea-I the raw
    // socket GET hangs when the whole small response arrives as one unsolicited
    // %SOCKETDATA URC. A bounded range (0 .. buf-1) also makes the modem strip
    // the HTTP headers, so the callback gets clean JSON body. The manifest is a
    // few hundred bytes; the range cap equals the fetch buffer. A small file
    // returns 206 Partial (or 200) -- both pass the 2xx check below.
    int http = s_range_fn(s_manifest_url, 0, sizeof(ctx.buf) - 1,
                          manifestFetchCb, &ctx);
    out.http_status = http;
    if (http < 200 || http >= 300) {
        out.status = ILABS_FOTA_CHECK_HTTP_FAIL;
        ilabs_fota__logf(3, "[fota-chk] manifest GET failed (status %d)", http);
        return false;
    }

    ilabs_fota_manifest_t m;
    if (ilabs_fota_manifest_parse(ctx.buf, ctx.len, &m) != 0) {
        out.status = ILABS_FOTA_CHECK_PARSE_FAIL;
        ilabs_fota__logf(3, "[fota-chk] manifest parse failed (%u B)",
                         (unsigned)ctx.len);
        return false;
    }

    out.version     = m.version;
    out.device_type = m.device_type;
    memcpy(out.version_str, m.version_str, sizeof(out.version_str));
    memcpy(out.url, m.url, sizeof(out.url));

    if (m.device_type != s_device_type) {
        out.status = ILABS_FOTA_CHECK_WRONG_DEVICE;
        ilabs_fota__logf(2, "[fota-chk] device_type 0x%04lX != mine 0x%04lX",
                         (unsigned long)m.device_type,
                         (unsigned long)s_device_type);
        return false;
    }

    if (m.version <= current_fw_version) {
        out.status = ILABS_FOTA_CHECK_UP_TO_DATE;
        ilabs_fota__logf(1, "[fota-chk] up to date (have 0x%08lX, offered 0x%08lX)",
                         (unsigned long)current_fw_version,
                         (unsigned long)m.version);
        return false;
    }

    out.update_available = true;
    out.status = ILABS_FOTA_CHECK_UPDATE_AVAILABLE;
    ilabs_fota__logf(1, "[fota-chk] update available 0x%08lX -> 0x%08lX (%s)",
                     (unsigned long)current_fw_version,
                     (unsigned long)m.version,
                     m.version_str[0] ? m.version_str : "?");
    return true;
}

bool iLabsDevMgmt::uploadLog(const char* url, const ilabs_log_source_t& src,
                               iLabsLogUploadResult& out) {
    // No session hooks here: the upload runs inside the caller's existing
    // modem session (the LTE task that powered + attached the modem and
    // holds the sleep lock), unlike update() which owns its own session.
    return ilabs_log_upload_run(url, &src, &out);
}

bool iLabsDevMgmt::triggerUpdate(uint32_t download_fw_version) {
    return FotaQspi.triggerUpdate(download_fw_version);
}
bool iLabsDevMgmt::readSettings(ilabs_fota_settings_t& out) const {
    return FotaQspi.readSettings(&out);
}

iLabsFotaQspi& iLabsDevMgmt::qspi() { return FotaQspi; }

// Pre-instantiated singleton.
iLabsDevMgmt DevMgmt;
iLabsDevMgmt& FOTA = DevMgmt;   // legacy alias (pre-0.5.0 sketches)
