#pragma once

/* ─── WiFi credentials ───────────────────────────────────────────── */
#define WIFI_SSID     "asdasd"
#define WIFI_PASSWORD "asdasdasd"

/* ─── OpenAI API ─────────────────────────────────────────────────── */
#define OPENAI_API_KEY       "asdasdas"
#define OPENAI_MODEL         "gpt-4o-mini-audio-preview"
#define OPENAI_MAX_TOKENS    150
#define OPENAI_MAX_CHARS     200

/* ─── Recording parameters ───────────────────────────────────────── */
#define RECORD_SAMPLE_RATE  16000
#define RECORD_CHANNELS     1
#define RECORD_BITS         16
#define RECORD_SECS         5
/* Total bytes: 16000 * 5 * 1 * 2 = 160 000 */
#define RECORD_BYTES        (RECORD_SAMPLE_RATE * RECORD_SECS * RECORD_CHANNELS * (RECORD_BITS / 8))

/* ─── Display (200×200 ePaper) ───────────────────────────────────── */
#define EPD_WIDTH   200
#define EPD_HEIGHT  200

/* ─── LVGL timing ────────────────────────────────────────────────── */
#define LVGL_TICK_MS       5
#define LVGL_MAX_DELAY_MS  500
#define LVGL_MIN_DELAY_MS  10
