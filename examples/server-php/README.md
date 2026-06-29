# Reference log-upload receiver (PHP)

A minimal server-side endpoint that receives the gzip-compressed device
logs this library uploads via `iLabsFotaClass::uploadLog()`. It is a
**reference implementation** — the library is deployment-agnostic (it owns
no backend), so a product can POST to anything that honours the wire
contract below. This PHP is a known-good starting point that matches the
library's framing exactly.

## Wire contract (must match `src/ilabs_log_upload.cpp`)

- **Request:** `POST /files/<product>/logs/{deveui}` — `{deveui}` is 16 hex
  chars (DevEUI, MSB-first). Body is a single gzip member (the compressed
  log delta).
- **Header:** `X-Log-SHA256: <hex>` — SHA-256 of the **compressed body**.
  The handler rejects a mismatch with `400` so the device retries (the
  Adrastea socket path has a known mid-stream byte-drop hazard).
- **Response:** `200` + JSON `{"received": N, ...}`, where `N` **must equal
  the body byte count**. The device only advances its upload watermark
  (`Log.markUploaded()`) when `received` matches what it sent — so a short
  write or truncation is detected and re-sent next time.

## Files

| File | Role |
|---|---|
| `index.php` | The handler: validate DevEUI, check `X-Log-SHA256`, store `{deveui}/{ISO8601}.log.gz`, reply `{"received":N}`. |
| `.htaccess` | Rewrites `/logs/{deveui}` → `index.php` (needs `mod_rewrite`). |
| `storage/.htaccess` | `Require all denied` — uploads must never be browsable. |
| `storage/.gitignore` | Keeps the dir + deny-rule, ignores uploaded artifacts. |

## Deploy

Drop this directory at your web root's log endpoint (e.g.
`/web-root/files/<product>/logs/`). `storage/` is created as a sibling of
`index.php` so the uploader can write without sudo. Retention is ad-hoc —
a cron such as `find storage -name '*.log.gz' -mtime +90 -delete`.

## FOTA manifest hosting

The pull-based update check (`checkForUpdate()`) needs **no PHP** — the
manifest is a static `manifest.json` and the firmware images are static
`*.slot.gz` files. Host them on any static/CDN path and point
`setManifestUrl()` at the manifest. See the top-level README's *Update
manifest* section for the JSON format.
