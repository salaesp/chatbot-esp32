#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "cJSON.h"

#include "lvgl.h"

#include "board_power_bsp.h"
#include "epaper_driver_bsp.h"
#include "audio_bsp.h"
#include "app_config.h"

#include "mbedtls/base64.h"

extern const uint8_t ca_pem_start[] asm("_binary_ca_pem_start");
extern const uint8_t ca_pem_end[]   asm("_binary_ca_pem_end");

static const char *TAG = "voice_transcription";

/* ─── ePaper driver (global for LVGL flush callback) ────────────── */
static epaper_driver_display *g_epd = NULL;

/* ─── LVGL ───────────────────────────────────────────────────────── */
static SemaphoreHandle_t s_lvgl_mux = NULL;
static lv_obj_t         *g_label    = NULL;

/* ─── WiFi ───────────────────────────────────────────────────────── */
static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0

/* ─── Recording buffer (allocated in PSRAM) ─────────────────────── */
static uint8_t *s_rec_buf = NULL;

/* ================================================================
 *  LVGL flush callback – converts LVGL 16-bit color → 1-bit ePaper
 * ================================================================ */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_map)
{
    uint16_t *buf = (uint16_t *)color_map;
    g_epd->EPD_Clear();
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t c = (*buf < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            g_epd->EPD_DrawColorPixel(x, y, c);
            buf++;
        }
    }
    g_epd->EPD_Display();          /* full refresh – clean & flicker-free */
    lv_disp_flush_ready(drv);
}

static void lvgl_tick_cb(void *) { lv_tick_inc(LVGL_TICK_MS); }

static void lvgl_task(void *arg)
{
    uint32_t delay_ms = LVGL_MAX_DELAY_MS;
    for (;;) {
        if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY)) {
            delay_ms = lv_timer_handler();
            xSemaphoreGive(s_lvgl_mux);
        }
        if (delay_ms > LVGL_MAX_DELAY_MS) delay_ms = LVGL_MAX_DELAY_MS;
        if (delay_ms < LVGL_MIN_DELAY_MS) delay_ms = LVGL_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* Map a Latin-1 Supplement code point (U+00A0–U+00FF) to ASCII.
 * Returns 0 to drop the character, otherwise the replacement byte. */
static char latin1_to_ascii(uint8_t cp)
{
    if (cp == 0xA1) return '!';           /* ¡ */
    if (cp == 0xBF) return '?';           /* ¿ */
    if (cp >= 0xC0 && cp <= 0xC5) return 'A'; /* À–Å */
    if (cp == 0xC7) return 'C';           /* Ç */
    if (cp >= 0xC8 && cp <= 0xCB) return 'E'; /* È–Ë */
    if (cp >= 0xCC && cp <= 0xCF) return 'I'; /* Ì–Ï */
    if (cp == 0xD0) return 'D';           /* Ð */
    if (cp == 0xD1) return 'N';           /* Ñ */
    if (cp >= 0xD2 && cp <= 0xD6) return 'O'; /* Ò–Ö */
    if (cp >= 0xD8 && cp <= 0xDD) return 'U'; /* Ø–Ý (close enough) */
    if (cp >= 0xE0 && cp <= 0xE5) return 'a'; /* à–å */
    if (cp == 0xE7) return 'c';           /* ç */
    if (cp >= 0xE8 && cp <= 0xEB) return 'e'; /* è–ë */
    if (cp >= 0xEC && cp <= 0xEF) return 'i'; /* ì–ï */
    if (cp == 0xF0) return 'd';           /* ð */
    if (cp == 0xF1) return 'n';           /* ñ */
    if (cp >= 0xF2 && cp <= 0xF6) return 'o'; /* ò–ö */
    if (cp >= 0xF8 && cp <= 0xFD) return 'u'; /* ø–ý */
    if (cp == 0xFF) return 'y';           /* ÿ */
    return '?';
}

/* Decode UTF-8 into the font's supported range (0x20–0x7E).
 * Writes into dst (null-terminated). dst must be at least strlen(src)+1 bytes. */
static void utf8_to_ascii(const char *src, char *dst, size_t dst_size)
{
    const uint8_t *s = (const uint8_t *)src;
    size_t di = 0;
    while (*s && di + 1 < dst_size) {
        if (*s < 0x80) {
            /* Plain ASCII */
            dst[di++] = (char)*s++;
        } else if ((*s & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            /* 2-byte UTF-8: U+0080–U+07FF */
            uint16_t cp = (uint16_t)((*s & 0x1F) << 6) | (s[1] & 0x3F);
            s += 2;
            if (cp >= 0xA0 && cp <= 0xFF) {
                char r = latin1_to_ascii((uint8_t)cp);
                if (r) dst[di++] = r;
            } else {
                dst[di++] = '?';
            }
        } else {
            /* 3- or 4-byte sequence: skip it */
            if ((*s & 0xF0) == 0xE0)      s += 3;
            else if ((*s & 0xF8) == 0xF0) s += 4;
            else                           s += 1;
            if (di + 1 < dst_size) dst[di++] = '?';
        }
    }
    dst[di] = '\0';
}

/* Update the label and force an immediate LVGL render. */
static void display_text(const char *text)
{
    if (!s_lvgl_mux || !g_label) return;
    /* Transliterate any UTF-8 characters outside the font's ASCII range */
    size_t len = strlen(text) + 1;
    char *buf = (char *)malloc(len);
    if (!buf) return;
    utf8_to_ascii(text, buf, len);
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(2000))) {
        lv_label_set_text(g_label, buf);
        lv_timer_handler();     /* triggers flush_cb → ePaper refresh */
        xSemaphoreGive(s_lvgl_mux);
    }
    free(buf);
}

/* ================================================================
 *  WiFi
 * ================================================================ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying…");
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                wifi_event_handler, NULL);

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID,     sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi connection…");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected");
}

/* ================================================================
 *  SNTP – sync system clock so TLS cert validation passes
 * ================================================================ */
static void sntp_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();

    /* Wait up to 20 s for COMPLETED status (not just != RESET) */
    int retries = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retries < 40) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retries++;
    }

    time_t now = 0;
    struct tm ti = {};
    time(&now);
    localtime_r(&now, &ti);
    int year = ti.tm_year + 1900;

    if (year < 2024) {
        ESP_LOGE(TAG, "SNTP sync FAILED – clock still at %04d, TLS will fail!", year);
    } else {
        ESP_LOGI(TAG, "SNTP OK: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 year, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
    }
}

/* ================================================================
 *  WAV header builder  (44-byte PCM RIFF)
 * ================================================================ */
static void build_wav_header(uint8_t *hdr, uint32_t pcm_bytes,
                             uint32_t sample_rate, uint16_t channels,
                             uint16_t bits)
{
    uint32_t byte_rate   = sample_rate * channels * bits / 8;
    uint16_t block_align = (uint16_t)(channels * bits / 8);
    uint32_t chunk_size  = pcm_bytes + 36;

    memcpy(hdr,      "RIFF", 4);
    memcpy(hdr + 4,  &chunk_size,  4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    *(uint32_t *)(hdr + 16) = 16;           /* fmt chunk size */
    *(uint16_t *)(hdr + 20) = 1;            /* PCM */
    *(uint16_t *)(hdr + 22) = channels;
    *(uint32_t *)(hdr + 24) = sample_rate;
    *(uint32_t *)(hdr + 28) = byte_rate;
    *(uint16_t *)(hdr + 32) = block_align;
    *(uint16_t *)(hdr + 34) = bits;
    memcpy(hdr + 36, "data", 4);
    *(uint32_t *)(hdr + 40) = pcm_bytes;
}

/* ================================================================
 *  Gemini — audio input → text response (single API call)
 *  Uses gemini-2.0-flash: transcribes + answers in one shot.
 *  WAV is base64-encoded and sent as an inline_data content part.
 * ================================================================ */
static char *ask_gemini(esp_http_client_handle_t client,
                        const uint8_t *pcm, size_t pcm_bytes)
{
    /* ── 1. Build contiguous WAV in PSRAM ── */
    uint8_t wav_hdr[44];
    build_wav_header(wav_hdr, (uint32_t)pcm_bytes,
                     RECORD_SAMPLE_RATE, RECORD_CHANNELS, RECORD_BITS);

    size_t   wav_size = sizeof(wav_hdr) + pcm_bytes;
    uint8_t *wav_buf  = (uint8_t *)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
    if (!wav_buf) { ESP_LOGE(TAG, "wav_buf alloc failed"); return NULL; }
    memcpy(wav_buf, wav_hdr, sizeof(wav_hdr));
    memcpy(wav_buf + sizeof(wav_hdr), pcm, pcm_bytes);

    /* ── 2. Base64-encode WAV into PSRAM ── */
    size_t b64_size;
    mbedtls_base64_encode(NULL, 0, &b64_size, wav_buf, wav_size);
    char *b64 = (char *)heap_caps_malloc(b64_size + 1, MALLOC_CAP_SPIRAM);
    if (!b64) {
        heap_caps_free(wav_buf);
        ESP_LOGE(TAG, "b64 buf alloc failed");
        return NULL;
    }
    size_t b64_out;
    mbedtls_base64_encode((unsigned char *)b64, b64_size + 1, &b64_out,
                          wav_buf, wav_size);
    b64[b64_out] = '\0';
    heap_caps_free(wav_buf);

    /* ── 3. JSON prefix & suffix (base64 data streamed in between) ── */
    char json_prefix[640];
    int prefix_len = snprintf(json_prefix, sizeof(json_prefix),
        "{\"system_instruction\":{\"parts\":[{\"text\":\"The audio contains a spoken question or message in Spanish. Respond in Spanish in under %d characters. Be concise and direct. Then add a blank line followed by a detailed multi-line ASCII art drawing (6-10 lines, max 20 chars wide) that clearly depicts an object, animal, or scene matching the topic. Use shading characters like / \\\\ | _ . ' ` ~ # @ to create recognizable shapes with depth. Avoid simple boxes. Use only basic ASCII characters (no unicode).\"}]},"
        "\"contents\":[{\"parts\":[{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"",
        GEMINI_MAX_CHARS);

    char json_suffix[64];
    int suffix_len = snprintf(json_suffix, sizeof(json_suffix),
        "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":%d}}", GEMINI_MAX_TOKENS);

    size_t total_body = (size_t)prefix_len + b64_out + (size_t)suffix_len;

    /* ── 4. POST to Gemini (API key in URL, no Auth header needed) ── */
    char url[512];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
        GEMINI_MODEL, GEMINI_API_KEY);

    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, (int)total_body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        heap_caps_free(b64);
        return NULL;
    }

    esp_http_client_write(client, json_prefix, prefix_len);
    esp_http_client_write(client, b64, (int)b64_out);
    esp_http_client_write(client, json_suffix, suffix_len);
    heap_caps_free(b64);

    /* ── 5. Read response ── */
    int content_len = esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Gemini HTTP %d  content-length=%d", status, content_len);

    const size_t RESP_BUF = 8192;
    char *resp = (char *)heap_caps_malloc(RESP_BUF, MALLOC_CAP_SPIRAM);
    if (!resp) { esp_http_client_close(client); return NULL; }

    int total_read = 0, n;
    while ((n = esp_http_client_read(client,
                                     resp + total_read,
                                     (int)(RESP_BUF - 1 - total_read))) > 0) {
        total_read += n;
    }
    resp[total_read] = '\0';
    ESP_LOGI(TAG, "Gemini response: %.256s", resp);

    esp_http_client_close(client);

    /* ── 6. Parse candidates[0].content.parts[0].text ── */
    cJSON *root = cJSON_Parse(resp);
    heap_caps_free(resp);

    if (!root) { ESP_LOGE(TAG, "JSON parse error"); return strdup("JSON parse error"); }

    char *result = NULL;
    cJSON *candidates = cJSON_GetObjectItem(root, "candidates");
    if (cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
        cJSON *parts = cJSON_GetObjectItem(
            cJSON_GetObjectItem(cJSON_GetArrayItem(candidates, 0), "content"), "parts");
        if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
            cJSON *text = cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 0), "text");
            if (cJSON_IsString(text) && text->valuestring[0] != '\0') {
                result = strdup(text->valuestring);
            }
        }
    }
    if (!result) {
        cJSON *api_msg = cJSON_GetObjectItem(
            cJSON_GetObjectItem(root, "error"), "message");
        char buf[256];
        snprintf(buf, sizeof(buf), "Error: %s",
                 cJSON_IsString(api_msg) ? api_msg->valuestring : "no response");
        result = strdup(buf);
    }

    cJSON_Delete(root);
    return result;
}

/* ================================================================
 *  app_main
 * ================================================================ */
extern "C" void app_main(void)
{
    /* ── 1. Power on rails ── */
    board_power_bsp_t power(
        6,   /* EPD_PWR_PIN   – active LOW  */
        42,  /* Audio_PWR_PIN – active LOW  */
        17   /* VBAT_PWR_PIN  – active HIGH */
    );
    power.POWEER_EPD_ON();
    power.POWEER_Audio_ON();
    power.VBAT_POWER_ON();
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ── 2. Init ePaper (SPI2_HOST) ── */
    custom_lcd_spi_t spi_cfg = {
        .cs         = 11,
        .dc         = 10,
        .rst        = 9,
        .busy       = 8,
        .mosi       = 13,
        .scl        = 12,
        .spi_host   = SPI2_HOST,
        .buffer_len = EPD_WIDTH * EPD_HEIGHT / 8,   /* 1 bpp */
    };
    g_epd = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, spi_cfg);
    g_epd->EPD_Init();
    g_epd->EPD_Clear();
    g_epd->EPD_Display();

    /* ── 3. Init LVGL ── */
    lv_init();

    lv_color_t *lvgl_buf1 = (lv_color_t *)heap_caps_malloc(
        EPD_WIDTH * EPD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_color_t *lvgl_buf2 = (lv_color_t *)heap_caps_malloc(
        EPD_WIDTH * EPD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(lvgl_buf1 && lvgl_buf2);

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf1, lvgl_buf2, EPD_WIDTH * EPD_HEIGHT);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res      = EPD_WIDTH;
    disp_drv.ver_res      = EPD_HEIGHT;
    disp_drv.flush_cb     = lvgl_flush_cb;
    disp_drv.draw_buf     = &draw_buf;
    disp_drv.full_refresh = 1;
    lv_disp_drv_register(&disp_drv);

    /* LVGL tick via esp_timer */
    esp_timer_create_args_t tick_args = {};
    tick_args.callback = lvgl_tick_cb;
    tick_args.name     = "lvgl_tick";
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    /* White background, centred black label with word-wrap */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    g_label = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(g_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_label, EPD_WIDTH - 10);
    lv_obj_align(g_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(g_label, lv_color_black(), 0);
    lv_label_set_text(g_label, "Booting...");

    s_lvgl_mux = xSemaphoreCreateMutex();
    assert(s_lvgl_mux);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);

    /* ── 4. WiFi ── */
    display_text("Connecting\nto WiFi...");
    wifi_init();
    display_text("Syncing\ntime...");
    sntp_sync();
    display_text("WiFi OK");
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* ── 5. Audio codec ── */
    audio_bsp_init();
    audio_play_init();   /* 16 kHz, stereo, 16-bit */

    /* ── 6. Persistent HTTPS client (standard CA store, no embedded cert) ── */
    esp_http_client_config_t http_cfg = {};
    http_cfg.url               = "https://generativelanguage.googleapis.com";
    http_cfg.cert_pem          = (const char *)ca_pem_start;
    http_cfg.timeout_ms        = 60000;
    http_cfg.buffer_size_tx    = 4096;
    http_cfg.keep_alive_enable = true;
    esp_http_client_handle_t http_client = esp_http_client_init(&http_cfg);
    assert(http_client);

    /* ── 7. Recording buffer in PSRAM ── */
    s_rec_buf = (uint8_t *)heap_caps_malloc(RECORD_BYTES, MALLOC_CAP_SPIRAM);
    assert(s_rec_buf);
    ESP_LOGI(TAG, "Recording buffer: %u bytes in PSRAM", RECORD_BYTES);

    /* ── 8. BOOT button (GPIO 0, active-low) ── */
    gpio_config_t btn = {};
    btn.intr_type    = GPIO_INTR_DISABLE;
    btn.mode         = GPIO_MODE_INPUT;
    btn.pin_bit_mask = (1ULL << GPIO_NUM_0);
    btn.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&btn);

    /* ── 9. Main loop ── */
    while (true) {
        display_text("Press BOOT\nbutton to\nrecord voice");

        /* Poll for button press (active-low) */
        while (gpio_get_level(GPIO_NUM_0) != 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(50));                   /* debounce */
        if (gpio_get_level(GPIO_NUM_0) != 0) continue;  /* spurious */

        /* ── Record ── */
        display_text("Recording...\n5 seconds");
        ESP_LOGI(TAG, "Recording %u bytes…", RECORD_BYTES);

        memset(s_rec_buf, 0, RECORD_BYTES);
        uint8_t  *ptr       = s_rec_buf;
        uint32_t  remaining = RECORD_BYTES;
        const uint32_t CHUNK = 1024;
        while (remaining > 0) {
            uint32_t n = (remaining > CHUNK) ? CHUNK : remaining;
            audio_playback_read(ptr, n);
            ptr       += n;
            remaining -= n;
        }
        ESP_LOGI(TAG, "Recording done");

        /* Wait for button release before transcribing */
        while (gpio_get_level(GPIO_NUM_0) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        /* ── Ask OpenAI (transcribe + answer in one call) ── */
        display_text("Thinking...");
        char *response = ask_gemini(http_client, s_rec_buf, RECORD_BYTES);

        if (response) {
            ESP_LOGI(TAG, "Response: %s", response);
            display_text(response);
            free(response);
        } else {
            display_text("Error:\nGemini failed");
        }

        /* Show result for 10 s, then loop */
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
