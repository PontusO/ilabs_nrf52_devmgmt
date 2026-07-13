// iLabs nRF52 Device Management -- iLabsFotaClass implementation + bridges.
//
// Holds the registered transport / log / session hooks and the runtime
// configuration as file-scope state, exposes them to the C-style
// orchestrator + self-test through the extern "C" ilabs_fota__* bridges
// declared in ilabs_fota_internal.h, and provides the C++ class methods
// the sketch uses.

#include "iLabs_nrf52_devmgmt.h"
#include "ilabs_fota_internal.h"

#include <Arduino.h>    // millis() for the heartbeat schedule
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

// Heartbeat schedule + config. Interval in ms (0 => disabled); s_hb_last_ms is
// the millis() of the last send (or of start), used for the wrap-safe due check.
static char                        s_hb_url[256]    = { 0 };
static ilabs_heartbeat_provider_fn s_hb_provider    = nullptr;
static void*                       s_hb_user        = nullptr;
static uint32_t                    s_hb_interval_ms = 0;
static uint32_t                    s_hb_last_ms     = 0;
static bool                        s_hb_enabled     = false;

// Cap the interval at 31 days so the millis()-based wrap-safe diff stays well
// inside the ~49.7-day millis() wrap. 0 disables.
static uint32_t hbHoursToMs(uint32_t hours) {
    if (hours == 0) return 0;
    if (hours > 744) hours = 744;   // 31 days
    return hours * 3600UL * 1000UL;
}

// True while the link "channel" is held open. A call that finds it already
// open neither re-fires begin nor closes it -- this is what lets
// postDiagnostic(ILABS_DIAG_KEEP_OPEN) keep the link up across several
// transactions (more diagnostics, and/or an uploadLog), and what lets
// update()/transportSelfTest() ride an already-open channel instead of
// double-opening it. begin/end are the onSessionBegin/onSessionEnd hooks.
static bool s_session_open = false;

static void sessionOpen(void) {
    if (!s_session_open) {
        if (s_begin_fn) s_begin_fn(s_begin_user);
        s_session_open = true;
    }
}
static void sessionClose(void) {
    if (s_session_open) {
        if (s_end_fn) s_end_fn(s_end_user);
        s_session_open = false;
    }
}

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
    bool owned = !s_session_open;   // only close a channel we opened here
    sessionOpen();
    bool ok = ilabs_fota_download(u, &out);
    if (owned) sessionClose();
    return ok;
}

bool iLabsDevMgmt::update(iLabsFotaResult& out) {
    return update(nullptr, out);
}

bool iLabsDevMgmt::transportSelfTest(const char* url, iLabsFotaTestResult& out) {
    const char* u = url ? url : ilabs_fota_test_url();
    bool owned = !s_session_open;
    sessionOpen();
    ilabs_fota_test_run(u, &out);
    if (owned) sessionClose();
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

// Effective diagnostic body cap: the transport's per-request cap (declared via
// setUploadTransport), or a safe default, bounded to the local build buffer.
static size_t diagBodyCap(void) {
    size_t cap = s_post_max ? s_post_max : 512;
    if (cap > 760) cap = 760;
    return cap;
}

// Build {"lvl":N,"msg":"<escaped>"} into buf (capacity `cap`), truncating the
// message if needed so the result is always VALID JSON within the cap (a diag
// body can't be shrink-retried like a log chunk). Returns the length, or 0.
static size_t buildDiagJson(char* buf, size_t cap, int level, const char* msg) {
    if (cap < 20) return 0;
    int n = snprintf(buf, cap, "{\"lvl\":%d,\"msg\":\"", level);
    if (n < 0 || (size_t) n >= cap) return 0;
    size_t o = (size_t) n;
    for (const char* p = msg; *p; ++p) {
        unsigned char c    = (unsigned char) *p;
        size_t        need = (c == '"' || c == '\\') ? 2 : 1;
        if (o + need + 2 >= cap) break;              // leave room for closing "}
        if (c == '"' || c == '\\') { buf[o++] = '\\'; buf[o++] = (char) c; }
        else if (c < 0x20)          { buf[o++] = ' '; }   // strip control chars
        else                         { buf[o++] = (char) c; }
    }
    buf[o++] = '"';
    buf[o++] = '}';
    buf[o]   = 0;
    return o;
}

// Shared core: manage the channel per `mode`, then POST `body` (already sized
// to the transport cap). No response body is read; retry only on transport
// failure (http <= 0) -- any HTTP status is terminal (best-effort; re-POST
// would risk a duplicate row). The transport appends its own &len=.
static int postDiagBody(const char* url, const uint8_t* body, size_t len,
                        ilabs_diag_mode_t mode) {
    if (!s_post_fn || len == 0) return -1;
    const bool close_after = (mode != ILABS_DIAG_KEEP_OPEN);
    sessionOpen();
    int http_status = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        http_status = s_post_fn(url, body, len, nullptr, nullptr, nullptr);
        if (http_status > 0) break;
    }
    if (close_after) sessionClose();
    return http_status;
}

int iLabsDevMgmt::postDiagnostic(const char* url, int level, const char* msg,
                                 ilabs_diag_mode_t mode) {
    if (!url || !msg || !s_post_fn) return -1;
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    char   body[768];
    size_t len = buildDiagJson(body, diagBodyCap(), level, msg);
    if (len == 0) return -1;
    return postDiagBody(url, (const uint8_t*) body, len, mode);
}

int iLabsDevMgmt::postDiagnosticJson(const char* url, const char* json,
                                     ilabs_diag_mode_t mode) {
    if (!url || !json || !s_post_fn) return -1;
    size_t len = strlen(json);
    // Reject an oversized bundle rather than truncate it -- a cut JSON body
    // would be invalid and rejected by the server anyway.
    if (len == 0 || len > diagBodyCap()) return -1;
    return postDiagBody(url, (const uint8_t*) json, len, mode);
}

void iLabsDevMgmt::closeDiagChannel() {
    sessionClose();
}

// Build the heartbeat bundle:
//   {"lvl":1,"msg":"heartbeat","fw":"M.m.p.r","devaddr":"0xXXXXXXXX","status":N[,"batt":X.XX]}
// Battery is formatted as integer centivolts (no %f -- the embedded newlib-nano
// printf is built without float support). Returns the length, or 0 on overflow.
static size_t buildHeartbeatJson(char* buf, size_t cap, const ilabs_heartbeat_t* hb) {
    int n;
    if (hb->has_battery) {
        int cv = (int)(hb->battery_volts * 100.0f + 0.5f);   // centivolts
        if (cv < 0) cv = 0;
        n = snprintf(buf, cap,
                     "{\"lvl\":1,\"msg\":\"heartbeat\",\"fw\":\"%lu.%lu.%lu.%lu\","
                     "\"devaddr\":\"0x%08lX\",\"status\":%lu,\"batt\":%d.%02d}",
                     (unsigned long)((hb->fw_version >> 24) & 0xFF),
                     (unsigned long)((hb->fw_version >> 16) & 0xFF),
                     (unsigned long)((hb->fw_version >>  8) & 0xFF),
                     (unsigned long)( hb->fw_version        & 0xFF),
                     (unsigned long)hb->devaddr,
                     (unsigned long)hb->status,
                     cv / 100, cv % 100);
    } else {
        n = snprintf(buf, cap,
                     "{\"lvl\":1,\"msg\":\"heartbeat\",\"fw\":\"%lu.%lu.%lu.%lu\","
                     "\"devaddr\":\"0x%08lX\",\"status\":%lu}",
                     (unsigned long)((hb->fw_version >> 24) & 0xFF),
                     (unsigned long)((hb->fw_version >> 16) & 0xFF),
                     (unsigned long)((hb->fw_version >>  8) & 0xFF),
                     (unsigned long)( hb->fw_version        & 0xFF),
                     (unsigned long)hb->devaddr,
                     (unsigned long)hb->status);
    }
    if (n < 0 || (size_t) n >= cap) return 0;
    return (size_t) n;
}

void iLabsDevMgmt::startHeartbeat(uint32_t interval_hours, const char* url,
                                  ilabs_heartbeat_provider_fn provider, void* user) {
    if (url) { strncpy(s_hb_url, url, sizeof(s_hb_url) - 1); s_hb_url[sizeof(s_hb_url) - 1] = 0; }
    else       s_hb_url[0] = 0;
    s_hb_provider    = provider;
    s_hb_user        = user;
    s_hb_interval_ms = hbHoursToMs(interval_hours);
    s_hb_last_ms     = millis();                 // first send one interval from now
    s_hb_enabled     = (s_hb_interval_ms != 0) && provider && (s_hb_url[0] != 0);
}

void iLabsDevMgmt::stopHeartbeat() {
    s_hb_enabled     = false;
    s_hb_interval_ms = 0;
}

bool iLabsDevMgmt::heartbeatEnabled() const {
    return s_hb_enabled;
}

void iLabsDevMgmt::setHeartbeatInterval(uint32_t interval_hours) {
    s_hb_interval_ms = hbHoursToMs(interval_hours);
    s_hb_last_ms     = millis();                 // re-base the schedule off now
    s_hb_enabled     = (s_hb_interval_ms != 0) && s_hb_provider && (s_hb_url[0] != 0);
}

bool iLabsDevMgmt::heartbeatDue() const {
    if (!s_hb_enabled || s_hb_interval_ms == 0) return false;
    return (uint32_t)(millis() - s_hb_last_ms) >= s_hb_interval_ms;
}

int iLabsDevMgmt::sendHeartbeat() {
    if (!s_hb_enabled || !s_hb_provider || !s_post_fn || s_hb_url[0] == 0) return -1;
    ilabs_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    s_hb_provider(&hb, s_hb_user);
    char   body[768];
    size_t len = buildHeartbeatJson(body, diagBodyCap(), &hb);
    if (len == 0) return -1;
    int http = postDiagBody(s_hb_url, (const uint8_t*) body, len, ILABS_DIAG_DIRECT);
    if (http > 0) s_hb_last_ms = millis();       // delivered -> re-base schedule
    return http;
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
