/*
 * TransportSelfTest -- verify the HTTPS byte transport in isolation.
 *
 * Downloads a known pattern file (byte[N] == N & 0xFF) and checks every
 * received byte. No QSPI write, no gunzip, no slot validation -- this
 * isolates the single question "is my modem/transport delivering bytes
 * faithfully?", which is the first thing to confirm when bringing up a
 * new transport before trusting it with a real firmware image.
 *
 * Generate the pattern file with a tiny script (byte[i] = i & 0xFF) and
 * host it on your server. As with BasicFota, the plain-GET adapter below
 * is a STUB -- forward it to your modem driver (the pingday firmware uses
 * lte_httpsGetSocket).
 */

#include <iLabs_nrf52_fota.h>

// Plain (non-ranged) HTTPS GET. Return the HTTP status, negative on error.
static int fotaPlainGet(const char* url, ilabs_fota_chunk_cb_t cb, void* user) {
  (void)url; (void)cb; (void)user;
  return -1;   // TODO: forward to your plain HTTPS GET implementation
}

static void fotaLog(int level, const char* msg, void* user) {
  (void)level; (void)user;
  Serial.println(msg);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  FOTA.setLogSink(fotaLog);
  FOTA.setTestTransport(fotaPlainGet);
  FOTA.begin();

  iLabsFotaTestResult t;
  bool pass = FOTA.transportSelfTest("https://example.com/test_pattern.bin", t);

  Serial.print("self-test: ");
  Serial.println(pass ? "PASS" : "FAIL");
  Serial.print("  bytes received: ");
  Serial.println((unsigned long)t.bytes_received);
  Serial.print("  mismatches:     ");
  Serial.println((unsigned long)t.mismatch_count);
  if (!pass && t.mismatch_count) {
    Serial.print("  first mismatch at offset: ");
    Serial.println((unsigned long)t.first_mismatch_off);
  }
}

void loop() {}
