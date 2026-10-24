#include "radio_catalog.h"
#include "radio_display_settings.h"
#include "fap_screenshot.h"
#include "radio_player.h"
#include "radio_sleep_timer.h"
#include "radio_ui.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace {

constexpr char kTag[] = "internet_radio";
struct KeyMessage {
    bsp_btn_t button;
    bsp_btn_ev_t event;
};

enum class ConfigInputMode {
    NetworkList,
    Keyboard,
    Connecting,
};

enum class SettingsInputMode {
    Menu,
    Volume,
    City,
    CityMatches,
    SleepTimer,
    Display,
    WifiMenu,
    SavedWifi,
    WifiAction,
    WifiDeleteConfirm,
};

struct CityChoice {
    const char *query;
    const char *display;
};

// The second field is what the dial shows. It is Chinese, like the rest of the
// interface; every glyph here is in the project font.
constexpr CityChoice kCityChoices[] = {
    {nullptr, "自动"},
    {"北京", "北京"},
    {"上海", "上海"},
    {"广州", "广州"},
    {"深圳", "深圳"},
    {"长沙", "长沙"},
    {"成都", "成都"},
};

constexpr std::size_t kCityChoiceCount = sizeof(kCityChoices) / sizeof(kCityChoices[0]);
constexpr uint8_t kCityChoiceStorageVersion = 2;

constexpr uint16_t kSleepTimerMinutes[] = {0, 15, 30, 60, 90};

constexpr std::size_t kMaxVisibleNetworks = 12;

QueueHandle_t s_key_queue;
std::atomic<bool> s_config_mode{false};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_settings_mode{false};
std::atomic<bool> s_sntp_started{false};
std::atomic<bool> s_config_can_cancel{false};
TaskHandle_t s_catalog_task;
SemaphoreHandle_t s_catalog_request;
StaticTask_t s_catalog_task_control;
StackType_t s_catalog_task_stack[6144 / sizeof(StackType_t)];
TaskHandle_t s_wifi_scan_task;
TaskHandle_t s_wifi_connect_task;
std::mutex s_wifi_ui_mutex;
ConfigInputMode s_config_input_mode = ConfigInputMode::NetworkList;
std::array<WifiNetworkView, kMaxVisibleNetworks> s_wifi_networks = {};
std::size_t s_wifi_network_count;
std::size_t s_wifi_network_selected;
std::string s_wifi_selected_ssid;
std::string s_wifi_password;
// The station search keyboard: what has been typed, its own key page, and
// whether it is open. Separate from the Wi-Fi keyboard so opening one never
// disturbs the other.
std::string s_search_query;
WifiKeyboardPage s_search_page = WifiKeyboardPage::Lower;
std::size_t s_search_key_selected;
std::atomic<bool> s_search_open{false};
WifiKeyboardPage s_wifi_keyboard_page = WifiKeyboardPage::Lower;
std::size_t s_wifi_key_selected;
std::string s_config_ap_ssid;
SettingsInputMode s_settings_input_mode = SettingsInputMode::Menu;
std::size_t s_settings_selected;
std::size_t s_saved_wifi_index;
std::mutex s_catalog_mutex;
uint32_t s_catalog_generation;
char s_requested_city[48];
char s_requested_city_display[48];
bool s_requested_resume_station;
// A typed search is a third kind of request, next to "this city" and "where am
// I". It is held beside them so the same generation counter retires an old one.
char s_requested_query[48];
constexpr std::size_t kMaxCityMatches = 8;
RadioCityMatch s_city_matches[kMaxCityMatches] = {};
const char *s_city_match_names[kMaxCityMatches] = {};
const char *s_city_match_provinces[kMaxCityMatches] = {};
std::size_t s_city_match_count;
// What the dial shows for the catalog now installed, so a search that finds
// nothing can put it back.
char s_active_location[48] = "中国";

void start_local_catalog_search();
void start_city_catalog_search(const char *city, const char *display,
                               bool resume_station = false);
void start_search(const char *query);
bool submit_city_search(const char *query);
void start_remembered_catalog_search();

// Persists the city the listener picked in Settings so a restart comes back to
// the same place instead of relocating from scratch every time.
void save_selected_city(std::size_t choice_index) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "city", static_cast<uint8_t>(choice_index));
    nvs_set_u8(handle, "city_version", kCityChoiceStorageVersion);
    // A built-in/manual choice supersedes a city found through text search.
    nvs_erase_key(handle, "city_name");
    nvs_commit(handle);
    nvs_close(handle);
}

void save_searched_city(const char *city) {
    if (!city || !city[0]) return;
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_str(handle, "city_name", city) == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(kTag, "Saved searched city '%s'", city);
    }
    nvs_close(handle);
}

bool load_searched_city(char *city, std::size_t capacity) {
    if (!city || capacity == 0) return false;
    city[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t length = capacity;
    const esp_err_t error = nvs_get_str(handle, "city_name", city, &length);
    nvs_close(handle);
    return error == ESP_OK && city[0];
}

bool searchable_city(const char *city) {
    RadioCityMatch match[1] = {};
    return city && city[0] && radio_catalog_find_cities(city, match, 1) > 0;
}

void clear_searched_city() {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_key(handle, "city_name");
    nvs_commit(handle);
    nvs_close(handle);
}

std::size_t load_selected_city() {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return 0;
    uint8_t value = 0;
    const bool has_city = nvs_get_u8(handle, "city", &value) == ESP_OK;
    uint8_t version = 0;
    const bool current =
        nvs_get_u8(handle, "city_version", &version) == ESP_OK &&
        version >= kCityChoiceStorageVersion;
    const std::size_t count = sizeof(kCityChoices) / sizeof(kCityChoices[0]);
    if (!current) {
        // Version 1 had Hangzhou at index 6 and Chengdu at index 7. Keep an
        // existing Hangzhou choice as a searched city, and shift Chengdu down
        // to its new default-list position.
        if (has_city && value == 6) {
            char searched[48] = {};
            std::size_t length = sizeof(searched);
            const bool has_search =
                nvs_get_str(handle, "city_name", searched, &length) == ESP_OK &&
                searched[0];
            if (!has_search) nvs_set_str(handle, "city_name", "杭州");
            value = 0;
        } else if (has_city && value == 7) {
            value = 6;
        } else if (!has_city || value >= count) {
            value = 0;
        }
        nvs_set_u8(handle, "city", value);
        nvs_set_u8(handle, "city_version", kCityChoiceStorageVersion);
        nvs_commit(handle);
    }
    nvs_close(handle);
    return value < count ? value : 0;
}

bool load_remembered_city_display(char *city, std::size_t capacity) {
    const std::size_t selected = load_selected_city();
    if (load_searched_city(city, capacity) && searchable_city(city)) return true;
    if (selected == 0 || selected >= kCityChoiceCount) return false;
    const char *display = kCityChoices[selected].display;
    if (!display || !display[0]) return false;
    std::snprintf(city, capacity, "%s", display);
    return true;
}

// Starts the search for whichever city was last chosen, falling back to
// automatic detection the first time the radio is used.
void start_remembered_catalog_search() {
    // Run the one-time index migration before checking city_name: an old
    // Hangzhou selection is converted into a searched city there.
    const std::size_t selected = load_selected_city();
    char searched[48] = {};
    if (load_searched_city(searched, sizeof(searched))) {
        if (searchable_city(searched)) {
            start_city_catalog_search(searched, searched, true);
            return;
        }
        ESP_LOGW(kTag, "Saved search city '%s' is no longer available", searched);
        clear_searched_city();
    }
    const CityChoice &choice = kCityChoices[selected];
    if (choice.query) {
        start_city_catalog_search(choice.query, choice.display, true);
    } else {
        start_local_catalog_search();
    }
}

// While up or down is held past the long-press threshold, the event repeats at
// this period so the value it drives ramps smoothly.
constexpr int kRepeatPeriodMs = 220;
// Keyboard repeats are deliberately slower. The grid uses six columns, so
// held up/down scrolls vertically one row.
constexpr int kKeyboardRepeatPeriodMs = 500;
constexpr std::size_t kKeyboardColumns = 6;

std::size_t keyboard_vertical_move(std::size_t selected, std::size_t count,
                                   int direction) {
    if (count == 0) return 0;
    selected %= count;
    const std::size_t row_count = (count + kKeyboardColumns - 1) / kKeyboardColumns;
    const std::size_t column = selected % kKeyboardColumns;
    int row = static_cast<int>(selected / kKeyboardColumns) + direction;
    if (row < 0) {
        row = static_cast<int>(row_count) - 1;
    } else if (row >= static_cast<int>(row_count)) {
        row = 0;
    }
    const std::size_t target = static_cast<std::size_t>(row) * kKeyboardColumns + column;
    // The symbol page has a short last row. Stay on its final key when the
    // current column has no cell there.
    return target < count ? target : count - 1;
}

int repeat_period_ms() {
    if (s_search_open.load(std::memory_order_acquire)) {
        return kKeyboardRepeatPeriodMs;
    }
    if (s_config_mode.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        if (s_config_input_mode == ConfigInputMode::Keyboard) {
            return kKeyboardRepeatPeriodMs;
        }
    }
    return kRepeatPeriodMs;
}

int button_from_mv(int millivolts) {
    if (millivolts < 0 || millivolts >= 1900) return -1;
    if (millivolts < 150) return BSP_BTN_UP;
    if (millivolts < 447) return BSP_BTN_DOWN;
    return BSP_BTN_OK;
}

void button_task(void *) {
    int candidate = -1;
    int stable = -1;
    uint8_t samples = 0;
    TickType_t pressed_at = 0;
    TickType_t repeat_at = 0;
    bool long_sent = false;

    while (true) {
        const int current = button_from_mv(bsp_button_read_mv());
        if (current == candidate) {
            if (samples < 3) ++samples;
        } else {
            candidate = current;
            samples = 1;
        }

        if (samples >= 3 && candidate != stable) {
            const int previous = stable;
            stable = candidate;
            if (stable >= 0) {
                pressed_at = xTaskGetTickCount();
                long_sent = false;
            } else if (previous >= 0 && !long_sent) {
                const KeyMessage message = {static_cast<bsp_btn_t>(previous), BSP_BTN_CLICK};
                if (xQueueSend(s_key_queue, &message, 0) != pdTRUE) {
                    ESP_LOGW(kTag, "key queue full; dropped a click");
                }
            }
        }

        if (stable >= 0 && !long_sent &&
            xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(850)) {
            const KeyMessage message = {static_cast<bsp_btn_t>(stable), BSP_BTN_LONG};
            if (xQueueSend(s_key_queue, &message, 0) != pdTRUE) {
                ESP_LOGW(kTag, "key queue full; dropped a long press");
            }
            long_sent = true;
            repeat_at = xTaskGetTickCount() + pdMS_TO_TICKS(repeat_period_ms());
        }

        // Up and down hold down to repeat, so volume and long lists ramp while
        // the key is held. OK must not repeat: its long press changes page.
        if (stable >= 0 && long_sent && stable != BSP_BTN_OK &&
            static_cast<int32_t>(xTaskGetTickCount() - repeat_at) >= 0) {
            const KeyMessage message = {static_cast<bsp_btn_t>(stable), BSP_BTN_LONG};
            xQueueSend(s_key_queue, &message, 0);
            repeat_at = xTaskGetTickCount() + pdMS_TO_TICKS(repeat_period_ms());
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void enter_wifi_config(bool clear_saved_networks, bool allow_cancel = false) {
    if (s_config_mode.load()) return;
    ESP_LOGI(kTag, "Entering Wi-Fi configuration mode");
    radio_player_set_network(false);
    if (clear_saved_networks) SsidManager::GetInstance().Clear();
    s_config_can_cancel.store(allow_cancel &&
                              !SsidManager::GetInstance().GetSsidList().empty());
    WifiManager::GetInstance().StartConfigAp();
}

void show_saved_wifi() {
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    std::array<const char *, 10> names = {};
    const std::size_t count = std::min(saved.size(), names.size());
    for (std::size_t i = 0; i < count; ++i) names[i] = saved[i].ssid.c_str();
    s_settings_selected = std::min(s_settings_selected, count);
    radio_ui_show_saved_wifi(names.data(), count, s_settings_selected);
}

std::string saved_wifi_ssid(std::size_t index) {
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    return index < saved.size() ? saved[index].ssid : std::string();
}

void show_wifi_action() {
    const std::string ssid = saved_wifi_ssid(s_saved_wifi_index);
    if (ssid.empty()) {
        s_settings_input_mode = SettingsInputMode::SavedWifi;
        s_settings_selected = 0;
        show_saved_wifi();
        return;
    }
    radio_ui_show_wifi_action(ssid.c_str(), s_settings_selected);
}

void show_wifi_delete_confirm() {
    const std::string ssid = saved_wifi_ssid(s_saved_wifi_index);
    if (ssid.empty()) {
        s_settings_input_mode = SettingsInputMode::SavedWifi;
        s_settings_selected = 0;
        show_saved_wifi();
        return;
    }
    radio_ui_show_wifi_delete_confirm(ssid.c_str(), s_settings_selected);
}

void show_wifi_list(bool scanning) {
    std::array<WifiNetworkView, kMaxVisibleNetworks> networks = {};
    std::size_t count = 0;
    std::size_t selected = 0;
    std::string setup_ssid;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        networks = s_wifi_networks;
        count = s_wifi_network_count;
        selected = count ? std::min(s_wifi_network_selected, count - 1) : 0;
        setup_ssid = s_config_ap_ssid;
    }
    radio_ui_show_wifi_list(networks.data(), count, selected, scanning,
                            setup_ssid.c_str(), s_config_can_cancel.load());
}

void show_wifi_keyboard(const char *status = nullptr) {
    std::string ssid;
    std::size_t password_length = 0;
    WifiKeyboardPage page;
    std::size_t selected = 0;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        ssid = s_wifi_selected_ssid;
        password_length = s_wifi_password.size();
        page = s_wifi_keyboard_page;
        selected = s_wifi_key_selected;
    }
    radio_ui_show_wifi_keyboard(ssid.c_str(), password_length, page, selected, status);
}

void wifi_scan_task(void *) {
    for (int attempt = 0; attempt < 16 && s_config_mode.load(); ++attempt) {
        auto access_points = WifiManager::GetInstance().GetConfigAccessPoints();
        if (!access_points.empty()) {
            std::sort(access_points.begin(), access_points.end(),
                      [](const wifi_ap_record_t &left, const wifi_ap_record_t &right) {
                          return left.rssi > right.rssi;
                      });
            std::array<WifiNetworkView, kMaxVisibleNetworks> networks = {};
            std::size_t count = 0;
            for (const auto &access_point : access_points) {
                const char *ssid = reinterpret_cast<const char *>(access_point.ssid);
                if (!ssid[0]) continue;
                bool duplicate = false;
                for (std::size_t i = 0; i < count; ++i) {
                    if (std::strncmp(networks[i].ssid, ssid,
                                     sizeof(networks[i].ssid)) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate || count >= networks.size()) continue;
                std::snprintf(networks[count].ssid, sizeof(networks[count].ssid),
                              "%.*s", 32, ssid);
                networks[count].rssi = access_point.rssi;
                networks[count].secure = access_point.authmode != WIFI_AUTH_OPEN;
                ++count;
            }
            if (count) {
                {
                    std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                    s_wifi_networks = networks;
                    s_wifi_network_count = count;
                    s_wifi_network_selected = 0;
                }
                show_wifi_list(false);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    bool show_empty_list = false;
    if (s_config_mode.load()) {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        show_empty_list = s_wifi_network_count == 0;
    }
    if (show_empty_list && s_config_mode.load()) show_wifi_list(false);
    s_wifi_scan_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_scan_ui() {
    if (s_wifi_scan_task) return;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_wifi_network_count = 0;
        s_wifi_network_selected = 0;
    }
    show_wifi_list(true);
    if (xTaskCreate(wifi_scan_task, "wifi_screen_scan", 4096, nullptr, 4,
                    &s_wifi_scan_task) != pdPASS) {
        s_wifi_scan_task = nullptr;
        show_wifi_list(false);
    }
}

void wifi_connect_task(void *) {
    std::string ssid;
    std::string password;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        ssid = s_wifi_selected_ssid;
        password = s_wifi_password;
    }
    const bool connected = WifiManager::GetInstance().ConfigureNetwork(ssid, password);
    if (!connected && s_config_mode.load()) {
        {
            std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
            s_config_input_mode = ConfigInputMode::Keyboard;
        }
        show_wifi_keyboard("连接失败");
    }
    s_wifi_connect_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_connection() {
    if (s_wifi_connect_task) return;
    std::string ssid;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        if (s_wifi_selected_ssid.empty()) return;
        s_config_input_mode = ConfigInputMode::Connecting;
        ssid = s_wifi_selected_ssid;
    }
    radio_ui_show_wifi_connecting(ssid.c_str(), "正在连接");
    if (xTaskCreate(wifi_connect_task, "wifi_screen_join", 5120, nullptr, 5,
                    &s_wifi_connect_task) != pdPASS) {
        s_wifi_connect_task = nullptr;
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_config_input_mode = ConfigInputMode::Keyboard;
    }
}

void handle_config_input(const KeyMessage &message) {
    ConfigInputMode mode;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        mode = s_config_input_mode;
    }
    if (mode == ConfigInputMode::Connecting) return;

    if (mode == ConfigInputMode::NetworkList) {
        if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
            if (s_config_can_cancel.load(std::memory_order_acquire)) {
                WifiManager::GetInstance().StopConfigAp();
            } else {
                start_wifi_scan_ui();
            }
            return;
        }
        bool redraw = false;
        bool open_keyboard = false;
        {
            std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
            if (s_wifi_network_count == 0) return;
            if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
                s_wifi_network_selected =
                    (s_wifi_network_selected + s_wifi_network_count - 1) %
                    s_wifi_network_count;
                redraw = true;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
                s_wifi_network_selected =
                    (s_wifi_network_selected + 1) % s_wifi_network_count;
                redraw = true;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
                s_wifi_selected_ssid = s_wifi_networks[s_wifi_network_selected].ssid;
                s_wifi_password.clear();
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
                s_config_input_mode = ConfigInputMode::Keyboard;
                open_keyboard = true;
            }
        }
        if (redraw) show_wifi_list(false);
        if (open_keyboard) show_wifi_keyboard();
        return;
    }

    if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
        start_wifi_connection();
        return;
    }

    bool redraw = false;
    bool connect = false;
    bool back_to_list = false;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        const std::size_t count = radio_ui_wifi_key_count(s_wifi_keyboard_page);
        if (!count) return;
        if (message.button == BSP_BTN_UP && message.event == BSP_BTN_CLICK) {
            s_wifi_key_selected = (s_wifi_key_selected + count - 1) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_DOWN && message.event == BSP_BTN_CLICK) {
            s_wifi_key_selected = (s_wifi_key_selected + 1) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_UP && message.event == BSP_BTN_LONG) {
            s_wifi_key_selected = keyboard_vertical_move(s_wifi_key_selected, count, -1);
            redraw = true;
        } else if (message.button == BSP_BTN_DOWN && message.event == BSP_BTN_LONG) {
            s_wifi_key_selected = keyboard_vertical_move(s_wifi_key_selected, count, 1);
            redraw = true;
        } else if (message.button == BSP_BTN_OK && message.event == BSP_BTN_CLICK) {
            const char *key = radio_ui_wifi_key(s_wifi_keyboard_page, s_wifi_key_selected);
            if (std::strcmp(key, "ABC") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Upper;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "abc") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "123") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Symbols;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "<") == 0) {
                if (!s_wifi_password.empty()) s_wifi_password.pop_back();
                else {
                    s_config_input_mode = ConfigInputMode::NetworkList;
                    back_to_list = true;
                }
            } else if (std::strcmp(key, "GO") == 0) {
                connect = true;
            } else if (s_wifi_password.size() < 63) {
                s_wifi_password += key;
            }
            redraw = true;
        }
    }
    if (back_to_list) show_wifi_list(false);
    else if (connect) start_wifi_connection();
    else if (redraw) show_wifi_keyboard();
}

// ── station search ──────────────────────────────────────────

void show_search_keyboard(const char *status = nullptr) {
    radio_ui_show_search_keyboard(s_search_query.c_str(), s_search_page,
                                  s_search_key_selected, status);
}

void open_station_search() {
    s_search_query.clear();
    s_search_page = WifiKeyboardPage::Lower;
    s_search_key_selected = 0;
    s_search_open = true;
    show_search_keyboard();
}

void close_station_search() {
    s_search_open = false;
    s_settings_input_mode = SettingsInputMode::City;
    s_settings_selected = kCityChoiceCount;  // back on the row that opened it
    radio_ui_show_settings(RadioSettingsPage::City, s_settings_selected,
                           radio_player_volume());
}

void handle_search_input(const KeyMessage &message) {
    const std::size_t count = radio_ui_wifi_key_count(s_search_page);
    if (!count) return;
    if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
        s_search_key_selected = (s_search_key_selected + count - 1) % count;
    } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
        s_search_key_selected = (s_search_key_selected + 1) % count;
    } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_UP) {
        s_search_key_selected = keyboard_vertical_move(s_search_key_selected, count, -1);
    } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_DOWN) {
        s_search_key_selected = keyboard_vertical_move(s_search_key_selected, count, 1);
    } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
        close_station_search();  // long press leaves, as everywhere else in settings
        return;
    } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
        const char *key = radio_ui_wifi_key(s_search_page, s_search_key_selected);
        if (std::strcmp(key, "ABC") == 0) {
            s_search_page = WifiKeyboardPage::Upper;
            s_search_key_selected = 0;
        } else if (std::strcmp(key, "abc") == 0) {
            s_search_page = WifiKeyboardPage::Lower;
            s_search_key_selected = 0;
        } else if (std::strcmp(key, "123") == 0) {
            s_search_page = WifiKeyboardPage::Symbols;
            s_search_key_selected = 0;
        } else if (std::strcmp(key, "<") == 0) {
            if (!s_search_query.empty()) {
                s_search_query.pop_back();
            } else {
                close_station_search();  // nothing left to delete: go back
                return;
            }
        } else if (std::strcmp(key, "GO") == 0) {
            if (s_search_query.empty()) {
                show_search_keyboard("请重试");
                return;
            }
            ESP_LOGI(kTag, "search submitted '%s'", s_search_query.c_str());
            if (!submit_city_search(s_search_query.c_str())) {
                show_search_keyboard("未找到城市 请重新输入");
                return;
            }
            s_search_open = false;
            return;
        } else if (s_search_query.size() < 32) {
            s_search_query += key;
        }
    } else {
        return;
    }
    show_search_keyboard();
}

void show_city_match_choices() {
    for (std::size_t i = 0; i < s_city_match_count; ++i) {
        s_city_match_names[i] = s_city_matches[i].name;
        s_city_match_provinces[i] = s_city_matches[i].province;
    }
    radio_ui_show_city_matches(s_city_match_names, s_city_match_provinces,
                               s_city_match_count, s_settings_selected);
}

bool submit_city_search(const char *query) {
    s_city_match_count =
        radio_catalog_find_cities(query, s_city_matches, kMaxCityMatches);
    if (s_city_match_count == 0) {
        ESP_LOGW(kTag, "City search '%s' matched no cities", query ? query : "");
        return false;
    }
    s_settings_mode.store(true);
    s_settings_input_mode = SettingsInputMode::CityMatches;
    s_settings_selected = 0;
    show_city_match_choices();
    return true;
}

void input_task(void *) {
    static constexpr const char *kButtonNames[] = {"up", "down", "ok"};
    static constexpr const char *kEventNames[] = {"press", "click", "double", "long"};
    KeyMessage message;
    while (true) {
        const BaseType_t received =
            xQueueReceive(s_key_queue, &message, pdMS_TO_TICKS(250));
        // Debounced NVS writes run here, outside the decoder task, whenever
        // button input is idle. Audio can preempt this task normally.
        radio_player_persist_pending_volume();
        if (received != pdTRUE) continue;
        ESP_LOGD(kTag, "key %s %s (settings=%d mode=%d selected=%u config=%d)",
                 kButtonNames[message.button], kEventNames[message.event],
                 static_cast<int>(s_settings_mode.load()),
                 static_cast<int>(s_settings_input_mode),
                 static_cast<unsigned>(s_settings_selected),
                 static_cast<int>(s_config_mode.load()));
        if (!radio_display_note_activity()) continue;
        if (s_config_mode.load(std::memory_order_acquire)) {
            handle_config_input(message);
            continue;
        }

        if (s_settings_mode.load(std::memory_order_acquire) && s_search_open) {
            handle_search_input(message);
            continue;
        }

        if (s_settings_mode.load(std::memory_order_acquire)) {
            const auto redraw_settings = []() {
                switch (s_settings_input_mode) {
                    case SettingsInputMode::Menu:
                        radio_ui_show_settings(RadioSettingsPage::Menu,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::Volume:
                        radio_ui_show_settings(RadioSettingsPage::Volume, 0,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::City:
                        radio_ui_show_settings(RadioSettingsPage::City,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::CityMatches:
                        show_city_match_choices();
                        break;
                    case SettingsInputMode::SleepTimer:
                        radio_ui_show_settings(RadioSettingsPage::SleepTimer,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::Display:
                        radio_ui_show_settings(RadioSettingsPage::Display,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::WifiMenu:
                        radio_ui_show_settings(RadioSettingsPage::WifiMenu,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::SavedWifi:
                        show_saved_wifi();
                        break;
                    case SettingsInputMode::WifiAction:
                        show_wifi_action();
                        break;
                    case SettingsInputMode::WifiDeleteConfirm:
                        show_wifi_delete_confirm();
                        break;
                }
            };

            if (s_settings_input_mode == SettingsInputMode::CityMatches) {
                if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
                    s_search_open = true;
                    show_search_keyboard();
                    continue;
                }

                const bool move = message.event == BSP_BTN_CLICK ||
                                  message.event == BSP_BTN_LONG;
                if (s_city_match_count > 0 && move &&
                    message.button == BSP_BTN_UP) {
                    s_settings_selected =
                        (s_settings_selected + s_city_match_count - 1) %
                        s_city_match_count;
                    redraw_settings();
                    continue;
                }
                if (s_city_match_count > 0 && move &&
                    message.button == BSP_BTN_DOWN) {
                    s_settings_selected =
                        (s_settings_selected + 1) % s_city_match_count;
                    redraw_settings();
                    continue;
                }
                if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK &&
                    s_city_match_count > 0) {
                    const char *city = s_city_matches[s_settings_selected].name;
                    ESP_LOGI(kTag, "city candidate selected '%s'", city);
                    s_settings_mode.store(false);
                    start_search(city);
                    radio_ui_show_main();
                }
                continue;
            }

            if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
                switch (s_settings_input_mode) {
                    case SettingsInputMode::Menu:
                        s_settings_mode.store(false);
                        radio_ui_show_main();
                        break;
                    case SettingsInputMode::Volume:
                    case SettingsInputMode::City:
                    case SettingsInputMode::SleepTimer:
                    case SettingsInputMode::Display:
                    case SettingsInputMode::WifiMenu: {
                        const std::size_t menu_selection =
                            s_settings_input_mode == SettingsInputMode::Volume ? 0 :
                            s_settings_input_mode == SettingsInputMode::City ? 1 :
                            s_settings_input_mode == SettingsInputMode::SleepTimer ? 2 :
                            s_settings_input_mode == SettingsInputMode::Display ? 3 : 4;
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = menu_selection;
                        redraw_settings();
                        break;
                    }
                    case SettingsInputMode::SavedWifi:
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 1;
                        redraw_settings();
                        break;
                    case SettingsInputMode::WifiAction:
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = s_saved_wifi_index;
                        redraw_settings();
                        break;
                    case SettingsInputMode::WifiDeleteConfirm:
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 1;
                        redraw_settings();
                        break;
                    case SettingsInputMode::CityMatches:
                        break;  // handled above
                }
                continue;
            }

            if (s_settings_input_mode == SettingsInputMode::Volume) {
                // A held key repeats, so the volume ramps instead of stepping
                // once per press.
                const bool step = message.event == BSP_BTN_CLICK ||
                                  message.event == BSP_BTN_LONG;
                if (step && message.button == BSP_BTN_UP) {
                    radio_player_set_volume(radio_player_volume() + 5);
                } else if (step && message.button == BSP_BTN_DOWN) {
                    const uint8_t volume = radio_player_volume();
                    radio_player_set_volume(volume > 15 ? volume - 5 : 10);
                } else if (message.event == BSP_BTN_CLICK &&
                           message.button == BSP_BTN_OK) {
                    s_settings_input_mode = SettingsInputMode::Menu;
                    s_settings_selected = 0;
                    radio_ui_show_settings(RadioSettingsPage::Menu, 0,
                                           radio_player_volume());
                }
                continue;
            }

            // The main menu and sleep timer have six rows; the city page now
            // has seven built-in choices plus its search row.
            std::size_t count = 6;
            if (s_settings_input_mode == SettingsInputMode::City) {
                count = kCityChoiceCount + 1;  // the cities, then 寻找电台
            }
            if (s_settings_input_mode == SettingsInputMode::Display ||
                s_settings_input_mode == SettingsInputMode::WifiMenu ||
                s_settings_input_mode == SettingsInputMode::WifiAction) {
                count = 3;
            } else if (s_settings_input_mode == SettingsInputMode::WifiDeleteConfirm) {
                count = 2;
            } else if (s_settings_input_mode == SettingsInputMode::SavedWifi) {
                count = SsidManager::GetInstance().GetSsidList().size() + 1;
            }
            // Up/down clicks move by one row; held-key repeats use LONG events
            // and must move through long lists as well.
            const bool move = message.event == BSP_BTN_CLICK ||
                              message.event == BSP_BTN_LONG;
            if (move && message.button == BSP_BTN_UP) {
                s_settings_selected = (s_settings_selected + count - 1) % count;
            } else if (move && message.button == BSP_BTN_DOWN) {
                s_settings_selected = (s_settings_selected + 1) % count;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
                if (s_settings_input_mode == SettingsInputMode::Menu) {
                    if (s_settings_selected == 0) {
                        s_settings_input_mode = SettingsInputMode::Volume;
                        radio_ui_show_settings(RadioSettingsPage::Volume, 0,
                                               radio_player_volume());
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::City;
                        s_settings_selected = load_selected_city();
                        radio_ui_show_settings(RadioSettingsPage::City,
                                               s_settings_selected,
                                               radio_player_volume());
                    } else if (s_settings_selected == 2) {
                        s_settings_input_mode = SettingsInputMode::SleepTimer;
                        s_settings_selected = radio_sleep_timer_selection();
                        radio_ui_show_settings(RadioSettingsPage::SleepTimer,
                                               s_settings_selected,
                                               radio_player_volume());
                    } else if (s_settings_selected == 3) {
                        s_settings_input_mode = SettingsInputMode::Display;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else if (s_settings_selected == 4) {
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else {
                        s_settings_mode.store(false);
                        radio_ui_show_main();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::SleepTimer) {
                    if (s_settings_selected < 5) {
                        radio_sleep_timer_set_minutes(
                            kSleepTimerMinutes[s_settings_selected]);
                    }
                    s_settings_input_mode = SettingsInputMode::Menu;
                    s_settings_selected = 2;
                    radio_ui_show_settings(RadioSettingsPage::Menu, 2,
                                           radio_player_volume());
                } else if (s_settings_input_mode == SettingsInputMode::City &&
                           s_settings_selected == kCityChoiceCount) {
                    open_station_search();
                } else if (s_settings_input_mode == SettingsInputMode::City &&
                           s_settings_selected < kCityChoiceCount) {
                    const CityChoice &choice = kCityChoices[s_settings_selected];
                    ESP_LOGI(kTag, "city selected index=%u query='%s'",
                             static_cast<unsigned>(s_settings_selected),
                             choice.query ? choice.query : "auto");
                    save_selected_city(s_settings_selected);
                    s_settings_mode.store(false);
                    if (choice.query) {
                        start_city_catalog_search(choice.query, choice.display);
                    } else {
                        start_local_catalog_search();
                    }
                    radio_ui_show_main();
                } else if (s_settings_input_mode == SettingsInputMode::Display) {
                    if (s_settings_selected == 0) {
                        radio_display_set_brightness_selection(
                            radio_display_brightness_selection() + 1);
                    } else if (s_settings_selected == 1) {
                        radio_display_set_timeout_selection(
                            radio_display_timeout_selection() + 1);
                    } else {
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 3;
                    }
                    redraw_settings();
                } else if (s_settings_input_mode == SettingsInputMode::WifiMenu) {
                    if (s_settings_selected == 0) {
                        s_settings_mode.store(false);
                        enter_wifi_config(false, true);
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else {
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 4;
                        redraw_settings();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::SavedWifi) {
                    const std::size_t saved_count =
                        SsidManager::GetInstance().GetSsidList().size();
                    if (s_settings_selected < saved_count) {
                        s_saved_wifi_index = s_settings_selected;
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else {
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 1;
                        redraw_settings();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::WifiAction) {
                    if (s_settings_selected == 0) {
                        SsidManager::GetInstance().SetDefaultSsid(
                            static_cast<int>(s_saved_wifi_index));
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::WifiDeleteConfirm;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else {
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = s_saved_wifi_index;
                        redraw_settings();
                    }
                } else if (s_settings_input_mode ==
                           SettingsInputMode::WifiDeleteConfirm) {
                    if (s_settings_selected == 1) {
                        SsidManager::GetInstance().RemoveSsid(
                            static_cast<int>(s_saved_wifi_index));
                        const std::size_t saved_count =
                            SsidManager::GetInstance().GetSsidList().size();
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected =
                            std::min(s_saved_wifi_index, saved_count);
                    } else {
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 1;
                    }
                    redraw_settings();
                }
                continue;
            }

            redraw_settings();
            continue;
        }

        if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
            radio_player_select_relative(-1);
        } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
            radio_player_select_relative(1);
        } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_UP) {
            radio_player_set_volume(radio_player_volume() + 5);
        } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_DOWN) {
            const uint8_t volume = radio_player_volume();
            radio_player_set_volume(volume > 15 ? volume - 5 : 10);
        } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
            radio_player_toggle();
        } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
            s_settings_mode.store(true);
            s_search_open = false;  // a search left open by a dropped connection
            s_settings_input_mode = SettingsInputMode::Menu;
            s_settings_selected = 0;
            radio_ui_show_settings(RadioSettingsPage::Menu, 0,
                                   radio_player_volume());
        }
    }
}

void start_sntp_once() {
    bool expected = false;
    if (!s_sntp_started.compare_exchange_strong(expected, true)) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.start = true;
    config.server_from_dhcp = false;
    config.renew_servers_after_new_IP = false;
    const esp_err_t error = esp_netif_sntp_init(&config);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(error));
        s_sntp_started.store(false);
    }
}

void catalog_task(void *) {
    constexpr std::size_t kDiscoveryCapacity = 8;
    while (true) {
        if (xSemaphoreTake(s_catalog_request, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI(kTag, "catalog worker woke");
        uint32_t generation = 0;
        char city[sizeof(s_requested_city)] = {};
        char display[sizeof(s_requested_city_display)] = {};
        char query[sizeof(s_requested_query)] = {};
        bool resume_station = false;
        {
            std::lock_guard<std::mutex> lock(s_catalog_mutex);
            generation = s_catalog_generation;
            std::snprintf(city, sizeof(city), "%s", s_requested_city);
            std::snprintf(display, sizeof(display), "%s", s_requested_city_display);
            std::snprintf(query, sizeof(query), "%s", s_requested_query);
            resume_station = s_requested_resume_station;
        }

        // Closing the old stream is what releases the HTTP client buffers.
        // Wait for that before allocating the directory response; switching
        // cities while a stream is still winding down otherwise fails on the
        // small ESP32-C3 heap and leaves the previous catalog visible.
        radio_player_wait_idle(3500);
        {
            std::lock_guard<std::mutex> lock(s_catalog_mutex);
            if (generation != s_catalog_generation) {
                continue;
            }
        }
        radio_player_release_decoder_memory();

        // The active stream and its 4-8 KiB jitter buffer must be gone before
        // this allocation. Allocating first can fail with a fragmented heap
        // and restart the old city instead of honoring the new request.
        auto *stations = static_cast<RadioStation *>(
            std::calloc(kDiscoveryCapacity, sizeof(RadioStation)));
        if (!stations) {
            ESP_LOGE(kTag, "Unable to allocate station catalog (free=%u largest=%u)",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(
                         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            if (s_connected.load(std::memory_order_acquire)) {
                radio_player_set_network(true);
            }
            continue;
        }
        std::memset(stations, 0, kDiscoveryCapacity * sizeof(RadioStation));
        ESP_LOGI(kTag, "catalog search starting (%s '%s'), internal heap free %u, largest %u",
                 query[0] ? "text" : "city", query[0] ? query : (city[0] ? city : "auto"),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        RadioLocation location = {};
        std::size_t count =
            query[0]  ? radio_catalog_search(query, stations, kDiscoveryCapacity, &location)
            : city[0] ? radio_catalog_discover_city(city, stations, kDiscoveryCapacity,
                                                    &location)
                      : radio_catalog_discover(stations, kDiscoveryCapacity, &location);

        // A city pick is an explicit request. If the first attempt lost a race
        // with the old stream teardown, retry once instead of leaving the old
        // city's stations on screen.
        if (count == 0 && (city[0] || query[0])) {
            vTaskDelay(pdMS_TO_TICKS(500));
            std::memset(stations, 0, kDiscoveryCapacity * sizeof(RadioStation));
            location = {};
            count = query[0]
                        ? radio_catalog_search(query, stations, kDiscoveryCapacity,
                                               &location)
                        : radio_catalog_discover_city(city, stations, kDiscoveryCapacity,
                                                      &location);
        }

        ESP_LOGI(kTag, "catalog search returned %u stations, internal heap free %u",
                 static_cast<unsigned>(count),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        if (generation != s_catalog_generation) {
            std::free(stations);
            stations = nullptr;
            continue;
        }
        if (s_connected.load(std::memory_order_acquire)) {
            // The dial names where the search was aimed: the located city, the
            // chosen city or the typed text. When a typed search finds nothing
            // the dial goes back to what is playing, and says so.
            // Startup, reconnect and automatic-location refreshes restore the
            // remembered station. A manual city pick or typed search starts
            // with the first station in that new catalog.
            const bool replaced =
                count > 0 &&
                (resume_station
                     ? radio_player_replace_stations_resuming(stations, count)
                     : radio_player_replace_stations(stations, count));
            if (replaced) {
                radio_ui_set_location(location.label[0] ? location.label : display);
                if (query[0]) {
                    save_searched_city(location.city[0] ? location.city : location.label);
                }
            } else if (city[0] || query[0]) {
                // Never label the old catalog with the newly selected city.
                // Keep the previous city catalog and resume it as the fallback.
                ESP_LOGW(kTag, "No stations for %s '%s'; restoring previous catalog",
                         query[0] ? "search" : "city",
                         query[0] ? query : city);
                radio_ui_set_location(s_active_location);
                radio_ui_set_playback(RadioPlaybackState::Buffering);
            } else {
                radio_ui_set_location(location.label[0] ? location.label : display);
                radio_ui_set_station(
                    radio_player_station_index(), radio_player_station_count(),
                    radio_player_station(radio_player_station_index()));
            }
            if (replaced) {
                std::snprintf(s_active_location, sizeof(s_active_location), "%s",
                              location.label[0] ? location.label : display);
            }
            // The player keeps its own copy. Return the catalog scratch buffer
            // before waking it so MP3 decoder setup does not compete with a
            // still-live search allocation for the remaining internal heap.
            std::free(stations);
            stations = nullptr;
            radio_player_set_network(true);
        }
        std::free(stations);
    }
}

void start_catalog_search(const char *city, const char *display,
                          bool resume_station) {
    {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        ++s_catalog_generation;
        std::snprintf(s_requested_city, sizeof(s_requested_city), "%s",
                      city ? city : "");
        std::snprintf(s_requested_city_display, sizeof(s_requested_city_display), "%s",
                      display ? display : "");
        s_requested_query[0] = '\0';
        s_requested_resume_station = resume_station;
    }
    radio_player_set_network(false);
    const BaseType_t queued = xSemaphoreGive(s_catalog_request);
    ESP_LOGI(kTag, "catalog request queued for %s '%s' (%s)",
             city && city[0] ? "city" : "auto", city && city[0] ? city : "auto",
             queued == pdTRUE ? "ok" : "already pending");
    radio_ui_set_location(city && city[0] ? display : "定位中");
    radio_ui_set_playback(RadioPlaybackState::Buffering,
                          city && city[0] ? "正在加载城市电台"
                                          : "正在搜索本地电台");
}

// Searches the directory for text typed on the keyboard. The dial and the
// station keep showing what is playing until the answer is in.
void start_search(const char *query) {
    {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        ++s_catalog_generation;
        s_requested_city[0] = '\0';
        s_requested_city_display[0] = '\0';
        s_requested_resume_station = false;
        std::snprintf(s_requested_query, sizeof(s_requested_query), "%s", query ? query : "");
    }
    radio_player_set_network(false);
    const BaseType_t queued = xSemaphoreGive(s_catalog_request);
    ESP_LOGI(kTag, "search request queued for '%s' (%s)",
             query ? query : "", queued == pdTRUE ? "ok" : "already pending");
    radio_ui_set_playback(RadioPlaybackState::Buffering, "正在寻找电台");
}

void start_local_catalog_search() {
    start_catalog_search(nullptr, nullptr, true);
}

void start_city_catalog_search(const char *city, const char *display,
                               bool resume_station) {
    start_catalog_search(city, display, resume_station);
}

void wifi_event(WifiEvent event, const std::string &data) {
    auto &manager = WifiManager::GetInstance();
    switch (event) {
        case WifiEvent::Scanning:
            radio_ui_set_network(false, "正在寻找网络");
            break;
        case WifiEvent::Connecting:
            radio_ui_set_network(false, data.c_str());
            radio_ui_set_playback(RadioPlaybackState::Stopped, "正在连接无线网络");
            break;
        case WifiEvent::Connected:
            ESP_LOGI(kTag, "Wi-Fi connected: %s, IP=%s", data.c_str(),
                     manager.GetIpAddress().c_str());
            s_connected.store(true);
            s_config_mode.store(false);
            s_config_can_cancel.store(false);
            s_settings_mode.store(false);
            radio_ui_show_main();
            radio_ui_set_network(true, data.c_str());
            manager.SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
            start_sntp_once();
            start_remembered_catalog_search();
            break;
        case WifiEvent::Disconnected:
            s_connected.store(false);
            radio_player_set_network(false);
            radio_ui_set_network(false, "网络已断开");
            break;
        case WifiEvent::ConfigModeEnter: {
            s_config_mode.store(true);
            s_settings_mode.store(false);
            s_connected.store(false);
            radio_player_set_network(false);
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_config_input_mode = ConfigInputMode::NetworkList;
                s_wifi_networks = {};
                s_wifi_network_count = 0;
                s_wifi_network_selected = 0;
                s_wifi_selected_ssid.clear();
                s_wifi_password.clear();
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
                s_config_ap_ssid = manager.GetApSsid();
            }
            start_wifi_scan_ui();
            break;
        }
        case WifiEvent::ConfigModeExit: {
            s_config_mode.store(false);
            s_config_can_cancel.store(false);
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_config_input_mode = ConfigInputMode::NetworkList;
                s_wifi_networks = {};
                s_wifi_network_count = 0;
                s_wifi_network_selected = 0;
                s_wifi_selected_ssid.clear();
                s_wifi_password.clear();
                s_wifi_key_selected = 0;
                s_config_ap_ssid.clear();
            }
            radio_ui_show_main();
            radio_ui_set_network(false, "正在连接网络");
            if (!manager.IsConnected()) manager.StartStation();
            break;
        }
    }
}

void connection_watchdog_task(void *) {
    while (true) {
        int waiting_seconds = 0;
        while (!s_connected.load() && !s_config_mode.load() && waiting_seconds < 30) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ++waiting_seconds;
        }
        if (!s_connected.load() && !s_config_mode.load()) {
            ESP_LOGW(kTag, "No usable network after 30 seconds; opening setup portal");
            enter_wifi_config(false);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    ESP_ERROR_CHECK(bsp_display_init());
    if (!bsp_lvgl_init()) {
        ESP_LOGE(kTag, "Display initialization failed");
        return;
    }
    if (!radio_display_settings_init()) {
        ESP_LOGW(kTag, "Display settings initialization failed");
    }
    bsp_battery_init();
    radio_catalog_set_renderable(radio_ui_can_render);
    if (!radio_ui_init()) {
        bsp_display_backlight(radio_display_brightness());
        ESP_LOGE(kTag, "UI initialization failed");
        return;
    }
    if (!radio_player_init()) {
        radio_ui_set_playback(RadioPlaybackState::Error, "音频初始化失败");
        bsp_display_backlight(radio_display_brightness());
        ESP_LOGE(kTag, "Audio player initialization failed");
        return;
    }
    char startup_city[sizeof(s_active_location)] = {};
    if (load_remembered_city_display(startup_city, sizeof(startup_city))) {
        std::snprintf(s_active_location, sizeof(s_active_location), "%s",
                      startup_city);
        radio_ui_set_location(startup_city);
    }
    // Rebuild once with the saved station and location, then flush both scene
    // halves before exposing the panel. First-time users keep the built-in
    // defaults because no saved city or station name is returned.
    radio_ui_show_main();
    radio_ui_refresh_now();
    bsp_display_backlight(radio_display_brightness());
    fap_screenshot_start();
    if (!radio_sleep_timer_init()) {
        ESP_LOGW(kTag, "Sleep timer initialization failed");
    }

    s_key_queue = xQueueCreate(10, sizeof(KeyMessage));
    ESP_ERROR_CHECK(bsp_button_init(nullptr, nullptr));
    if (!s_key_queue ||
        xTaskCreate(button_task, "radio_keys", 2048, nullptr, 6, nullptr) != pdPASS ||
        xTaskCreate(input_task, "radio_input", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Button initialization failed");
        return;
    }

    s_catalog_request = xSemaphoreCreateBinary();
    if (!s_catalog_request) {
        ESP_LOGE(kTag, "Catalog request semaphore failed");
        return;
    }
    s_catalog_task = xTaskCreateStatic(
        catalog_task, "radio_catalog",
        sizeof(s_catalog_task_stack) / sizeof(s_catalog_task_stack[0]),
        nullptr, 5, s_catalog_task_stack, &s_catalog_task_control);
    if (!s_catalog_task) {
        ESP_LOGE(kTag, "Catalog worker task failed");
        return;
    }
    auto &manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "OCTO Radio";
    config.fixed_ap_ssid = "OCTO Radio";
    config.language = "zh-CN";
    config.station_hostname = "octo-radio";
    config.station_scan_min_interval_seconds = 5;
    config.station_scan_max_interval_seconds = 30;
    if (!manager.Initialize(config)) {
        radio_ui_set_playback(RadioPlaybackState::Error, "无线网络初始化失败");
        return;
    }
    manager.SetEventCallback(wifi_event);

    auto &ssid_manager = SsidManager::GetInstance();
    const auto &saved_networks = ssid_manager.GetSsidList();
    if (saved_networks.empty()) {
        // Public/share builds contain no owner's Wi-Fi credentials. A fresh
        // device therefore opens its setup portal immediately.
        manager.StartConfigAp();
    } else {
        // Existing owners keep all networks already stored in NVS. Give those
        // networks 30 seconds to appear before opening the setup portal.
        manager.StartStation();
    }

    if (xTaskCreate(connection_watchdog_task, "radio_wifi_guard", 3072, nullptr, 4,
                    nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Wi-Fi watchdog task failed");
    }
    ESP_LOGI(kTag, "Internet radio ready");
}
