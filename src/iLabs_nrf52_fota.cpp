// iLabs nRF52 FOTA -- iLabsFotaClass implementation + internal bridges.
//
// Holds the registered transport / log / session hooks and the runtime
// configuration as file-scope state, exposes them to the C-style
// orchestrator + self-test through the extern "C" ilabs_fota__* bridges
// declared in ilabs_fota_internal.h, and provides the C++ class methods
// the sketch uses.

#include "iLabs_nrf52_fota.h"
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

static ilabs_fota_log_fn             s_log_fn    = nullptr;
static void*                         s_log_user  = nullptr;

static ilabs_fota_session_fn         s_begin_fn  = nullptr;
static void*                         s_begin_user = nullptr;
static ilabs_fota_session_fn         s_end_fn    = nullptr;
static void*                         s_end_user  = nullptr;

static char     s_url[256]     = { 0 };
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
bool iLabsFotaClass::begin() {
    bool ok = FotaQspi.begin();
    ilabs_fota_gunzip_init();
    return ok;
}

void iLabsFotaClass::suspend()       { FotaQspi.suspend(); }
void iLabsFotaClass::resume()        { FotaQspi.resume(); }
bool iLabsFotaClass::ready() const   { return FotaQspi.ready(); }

void iLabsFotaClass::setFirmwareUrl(const char* url) {
    if (!url) { s_url[0] = '\0'; return; }
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
}

void iLabsFotaClass::setDeviceType(uint32_t dev_type) { s_device_type = dev_type; }

void iLabsFotaClass::setMegachunkSize(size_t bytes) {
    if (bytes) s_megachunk = bytes;
}

void iLabsFotaClass::setTransport(ilabs_fota_https_get_range_fn range_get) {
    s_range_fn = range_get;
}
void iLabsFotaClass::setTestTransport(ilabs_fota_https_get_fn plain_get) {
    s_plain_fn = plain_get;
}
void iLabsFotaClass::setUploadTransport(ilabs_https_post_fn post_fn) {
    s_post_fn = post_fn;
}

void iLabsFotaClass::setLogSink(ilabs_fota_log_fn fn, void* user) {
    s_log_fn = fn; s_log_user = user;
}
void iLabsFotaClass::onSessionBegin(ilabs_fota_session_fn fn, void* user) {
    s_begin_fn = fn; s_begin_user = user;
}
void iLabsFotaClass::onSessionEnd(ilabs_fota_session_fn fn, void* user) {
    s_end_fn = fn; s_end_user = user;
}

bool iLabsFotaClass::update(const char* url, iLabsFotaResult& out) {
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

bool iLabsFotaClass::update(iLabsFotaResult& out) {
    return update(nullptr, out);
}

bool iLabsFotaClass::transportSelfTest(const char* url, iLabsFotaTestResult& out) {
    const char* u = url ? url : ilabs_fota_test_url();
    if (s_begin_fn) s_begin_fn(s_begin_user);
    ilabs_fota_test_run(u, &out);
    if (s_end_fn) s_end_fn(s_end_user);
    return out.pass;
}

bool iLabsFotaClass::uploadLog(const char* url, const ilabs_log_source_t& src,
                               iLabsLogUploadResult& out) {
    // No session hooks here: the upload runs inside the caller's existing
    // modem session (the LTE task that powered + attached the modem and
    // holds the sleep lock), unlike update() which owns its own session.
    return ilabs_log_upload_run(url, &src, &out);
}

bool iLabsFotaClass::triggerUpdate(uint32_t download_fw_version) {
    return FotaQspi.triggerUpdate(download_fw_version);
}
bool iLabsFotaClass::confirmBoot(uint32_t current_app_fw_version) {
    return FotaQspi.confirmBoot(current_app_fw_version);
}
bool iLabsFotaClass::readSettings(ilabs_fota_settings_t& out) const {
    return FotaQspi.readSettings(&out);
}

iLabsFotaQspi& iLabsFotaClass::qspi() { return FotaQspi; }

// Pre-instantiated singleton.
iLabsFotaClass FOTA;
