// Host stub for exercising the catalog's request path without a network.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#define ESP_OK 0
typedef struct {
    const char *url;
    int timeout_ms, buffer_size, buffer_size_tx;
    const char *user_agent;
    bool keep_alive_enable, disable_auto_redirect;
    int max_redirection_count;
    int (*crt_bundle_attach)(void *);
} esp_http_client_config_t;

struct CatalogHttpStub {
    std::size_t position = 0;
    std::string url;
};

inline std::string catalog_test_response;
inline std::string catalog_test_last_url;

typedef CatalogHttpStub *esp_http_client_handle_t;
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    static CatalogHttpStub client;
    client.position = 0;
    client.url = config && config->url ? config->url : "";
    catalog_test_last_url = client.url;
    return &client;
}
inline int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *) { return ESP_OK; }
inline int esp_http_client_open(esp_http_client_handle_t client, int) {
    if (!client) return -1;
    client->position = 0;
    return ESP_OK;
}
inline long esp_http_client_fetch_headers(esp_http_client_handle_t) {
    return static_cast<long>(catalog_test_response.size());
}
inline int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
inline int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int capacity) {
    if (!client || !buffer || capacity <= 0) return -1;
    const std::size_t remaining = catalog_test_response.size() - client->position;
    const std::size_t count = std::min<std::size_t>(remaining, static_cast<std::size_t>(capacity));
    if (count) {
        std::memcpy(buffer, catalog_test_response.data() + client->position, count);
        client->position += count;
    }
    return static_cast<int>(count);
}
inline int esp_http_client_close(esp_http_client_handle_t) { return 0; }
inline int esp_http_client_cleanup(esp_http_client_handle_t) { return 0; }
