#include "spiffs_www.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "SPIFFS";

/* ── Mime types ──────────────────────────────────────────────────────────── */
static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    /* Ignoruj .gz – sprawdzaj rozszerzenie bazowe */
    char clean[64] = {};
    strncpy(clean, path, sizeof(clean) - 1);
    char *gz = strstr(clean, ".gz");
    if (gz) *gz = '\0';
    ext = strrchr(clean, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".woff2")== 0) return "font/woff2";
    return "application/octet-stream";
}

/* ── Mount ───────────────────────────────────────────────────────────────── */
esp_err_t spiffs_www_mount(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/www",
        .partition_label        = "spiffs",
        .max_files              = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        size_t total = 0, used = 0;
        esp_spiffs_info("spiffs", &total, &used);
        ESP_LOGI(TAG, "SPIFFS zamontowany /www  total=%u used=%u", total, used);
    } else {
        ESP_LOGE(TAG, "SPIFFS mount błąd 0x%x", err);
    }
    return err;
}

esp_err_t spiffs_www_unmount(void) {
    return esp_vfs_spiffs_unregister("spiffs");
}

/* ── Serve file ──────────────────────────────────────────────────────────── */
esp_err_t spiffs_www_serve(httpd_req_t *req, const char *path) {
    /* Spróbuj najpierw wersję .gz */
    char gz_path[128];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", path);

    struct stat st;
    bool is_gz  = (stat(gz_path, &st) == 0);
    const char *open_path = is_gz ? gz_path : path;

    if (!is_gz && stat(path, &st) != 0) {
        ESP_LOGW(TAG, "Plik nie znaleziony: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    FILE *f = fopen(open_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fopen failed");
        return ESP_FAIL;
    }

    /* Nagłówki */
    httpd_resp_set_type(req, mime_type(path));
    if (is_gz)
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");

    /* Strumieniuj plik */
    char chunk[1024];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)r) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, NULL);  /* zakończ chunked transfer */
    ESP_LOGD(TAG, "Serwowano: %s (%s, %ld B)",
             path, is_gz ? "gzip" : "raw", (long)st.st_size);
    return ESP_OK;
}
