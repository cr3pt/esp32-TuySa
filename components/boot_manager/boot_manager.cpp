#include "boot_manager.h"
#include "config_manager.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BOOT";
static boot_mode_t s_mode = BOOT_MODE_SETUP;

/* ── Opis przyczyny resetu ──────────────────────────────────────────────── */
static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_DEEPSLEEP:return "deep-sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        default:               return "other";
    }
}

/* ── Inicjalizacja NVS ──────────────────────────────────────────────────── */
static void nvs_init_safe(void) {
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS wymaga formatowania (err=0x%x) – kasuję partycję", err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS gotowe");
}

/* ── Informacje o chipie ────────────────────────────────────────────────── */
static void log_chip_info(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "Chip: cores=%d, flash=%s, WiFi=%s, BT=%s",
        chip.cores,
        (chip.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external",
        (chip.features & CHIP_FEATURE_WIFI_BGN)  ? "yes" : "no",
        (chip.features & CHIP_FEATURE_BT)         ? "yes" : "no");
}

/* ── API publiczne ──────────────────────────────────────────────────────── */
boot_mode_t boot_manager_init(void) {
    /* 1. Loguj przyczynę resetu */
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "=== Uruchomienie systemu | przyczyna: %s ===",
             reset_reason_str(reason));
    log_chip_info();

    /* 2. Inicjalizuj NVS */
    nvs_init_safe();

    /* 3. Inicjalizuj menedżer konfiguracji */
    esp_err_t err = config_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_manager_init() błąd 0x%x – tryb SETUP", err);
        s_mode = BOOT_MODE_SETUP;
        return s_mode;
    }

    /* 4. Sprawdź czy konfiguracja sieciowa jest kompletna */
    if (!config_manager_is_provisioned()) {
        ESP_LOGW(TAG, "Brak konfiguracji WiFi – tryb SETUP (Captive Portal)");
        s_mode = BOOT_MODE_SETUP;
    } else {
        ESP_LOGI(TAG, "Konfiguracja kompletna – tryb RUNTIME");
        s_mode = BOOT_MODE_RUNTIME;
    }

    return s_mode;
}

boot_mode_t boot_manager_get_mode(void) {
    return s_mode;
}

void boot_manager_restart(const char *reason, uint32_t delay_ms) {
    ESP_LOGW(TAG, "Restart zaplanowany: %s (za %lu ms)", reason, (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}
