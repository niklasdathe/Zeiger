// secrets_example.h  — template (SAFE to commit)
#pragma once

// ---- WiFi ----
#define SECRET_WIFI_SSID "MyWifi"
#define SECRET_WIFI_PASS "SuperSecret123"

// ---- Calendar URL (treat like a password) ----
#define SECRET_CAL_URL   "https://calendar.google.com/calendar/ical/REDACTED/basic.ics"

// Optional: set to 1 to allow insecure TLS (not recommended for production)
#define SECRET_INSECURE_TLS 1
