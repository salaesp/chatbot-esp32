#pragma once

/* ─── WiFi credentials ───────────────────────────────────────────── */
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

/* ─── Gemini API ─────────────────────────────────────────────────── */
#define GEMINI_API_KEY    ""
#define GEMINI_MODEL      "gemini-2.5-flash"
#define GEMINI_MAX_TOKENS 1500
#define GEMINI_MAX_CHARS  300

/* ─── Recording parameters ───────────────────────────────────────── */
#define RECORD_SAMPLE_RATE  16000
#define RECORD_CHANNELS     2
#define RECORD_BITS         16
#define RECORD_SECS         5
/* Total bytes: 16000 * 5 * 2 * 2 = 320 000 */
#define RECORD_BYTES        (RECORD_SAMPLE_RATE * RECORD_SECS * RECORD_CHANNELS * (RECORD_BITS / 8))

/* ─── Display (200×200 ePaper) ───────────────────────────────────── */
#define EPD_WIDTH   200
#define EPD_HEIGHT  200

/* ─── LVGL timing ────────────────────────────────────────────────── */
#define LVGL_TICK_MS       5
#define LVGL_MAX_DELAY_MS  500
#define LVGL_MIN_DELAY_MS  10
