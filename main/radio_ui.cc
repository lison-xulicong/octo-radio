#include "radio_ui.h"

#include "radio_display_settings.h"
#include "radio_header.h"
#include "radio_octopus.h"
#include "radio_sleep_timer.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/display/lv_display_private.h"
#include "radio_font.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr uint32_t kBackground = 0x071017;
constexpr uint32_t kPanel = 0x0D1B23;
constexpr uint32_t kPanelSoft = 0x11262E;
constexpr uint32_t kGrid = 0x24404A;
constexpr uint32_t kText = 0xF4F0DE;
constexpr uint32_t kMuted = 0x849BA0;
constexpr uint32_t kAmber = 0xFFB74D;
constexpr uint32_t kAmberSoft = 0x7F5A29;
constexpr uint32_t kGreen = 0x4ED39A;
constexpr uint32_t kRed = 0xFF5D62;

// Dial geometry. A valid FM frequency maps directly onto the 88-108 rail;
// stations without one use their catalog position.
constexpr int kDialBandLow = 880;
constexpr int kDialBandHigh = 1080;
constexpr int kDialTrackLeft = 16;
constexpr int kDialTrackRight = 198;
constexpr int kDialNeedleWidth = 3;
constexpr int kDialNeedleHome = (kDialTrackLeft + kDialTrackRight) / 2;

// Main page. The octopus scene owns panel rows kOctopusPanelTop..+Height and
// is drawn outside LVGL, so LVGL draws nothing there. The header and the
// channel row on top of it are painted into the scene's overlay instead.
constexpr int kStationNameY = 191;
constexpr int kStationDescriptionY = 210;
constexpr int kDialY = 230;
constexpr int kDialH = 48;
constexpr int kDialTrackY = 26;
constexpr int kNeedleY = 16;
constexpr int kNeedleH = 18;
constexpr int kGlowY = 23;
constexpr int kMeterY = 281;
constexpr int kMeterH = 14;
constexpr int kStatusY = 298;
constexpr uint32_t kDeepWaterTop = 0x162c6e;     // scene bottom row (palette bg3)
constexpr uint32_t kDeepWaterBottom = 0x06082a;  // palette bg5
constexpr std::size_t kMeterCount = 18;
constexpr std::size_t kWifiVisibleRows = 6;
constexpr std::size_t kWifiMaxKeys = 34;
// The city list and search results currently max out at eight rows.
constexpr std::size_t kSettingsMaxRows = 8;

enum class Page {
    Main,
    Settings,
    Config,
    WifiList,
    WifiKeyboard,
    WifiConnecting,
    SearchKeyboard,
};

constexpr const char *kLowerKeys[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
    "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
    "y", "z", "ABC", "123", "<", "GO",
};
constexpr const char *kUpperKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",
    "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "abc", "123", "<", "GO",
};
constexpr const char *kSymbolKeys[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "_",
    ".", "@", "#", "$", "%", "&", "*", "!", "+", "=", "?", "/",
    ":", ";", "(", ")", "[", "]", "abc", "ABC", "<", "GO",
};

lv_obj_t *s_screen;
lv_obj_t *s_main_group;
lv_obj_t *s_station_name;
lv_obj_t *s_station_description;
lv_obj_t *s_play_icon;
lv_obj_t *s_status;
lv_obj_t *s_location;
lv_obj_t *s_meter[kMeterCount];
lv_obj_t *s_meter_panel;
lv_obj_t *s_main_volume_caption;
lv_obj_t *s_main_volume;
lv_obj_t *s_volume_value;
lv_obj_t *s_volume_bar;
lv_obj_t *s_settings_rows[kSettingsMaxRows];
lv_obj_t *s_settings_labels[kSettingsMaxRows];
lv_obj_t *s_pair_dot;
lv_obj_t *s_dial_needle;
lv_obj_t *s_dial_glow;
lv_obj_t *s_dial_readout;
lv_obj_t *s_wifi_state;
lv_obj_t *s_wifi_list_hint;
lv_obj_t *s_wifi_row_box[kWifiVisibleRows];
lv_obj_t *s_wifi_row_ssid[kWifiVisibleRows];
lv_obj_t *s_wifi_row_info[kWifiVisibleRows];
lv_obj_t *s_wifi_keyboard_network;
lv_obj_t *s_wifi_password_mask;
lv_obj_t *s_wifi_keyboard_status;
lv_obj_t *s_wifi_key_box[kWifiMaxKeys];
lv_obj_t *s_wifi_key_label[kWifiMaxKeys];

Page s_page = Page::Main;
bool s_network_connected;
uint8_t s_volume = 55;
RadioSettingsPage s_settings_page = RadioSettingsPage::Menu;
std::size_t s_settings_selected;
std::size_t s_settings_row_count;
std::size_t s_station_index;
std::size_t s_station_count = 1;
char s_station_name_text[64] = "清晨音乐";
char s_station_description_text[96] = "轻松音乐 · 网络直播";
char s_network_detail[64] = "等待网络";
char s_location_text[48] = "中国";
std::atomic<RadioPlaybackState> s_playback_state{RadioPlaybackState::Stopped};
std::atomic<uint32_t> s_playback_generation{1};
uint32_t s_last_playback_generation;
std::atomic<uint32_t> s_level_generation{0};
uint8_t s_levels[kMeterCount] = {};
uint32_t s_last_level_generation;
uint8_t s_animation;
uint32_t s_battery_ticks;
uint32_t s_clock_ticks = 20;
uint32_t s_last_sleep_seconds = UINT32_MAX;
uint16_t s_station_frequency;
int32_t s_needle_x = kDialNeedleHome;

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                   uint32_t color, int radius = 0) {
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    return object;
}

// Defined below, next to the scene timer: renders one octopus frame straight
// to the panel. build_main() calls it so a rebuilt page never shows LVGL's
// background where the scene belongs.
bool draw_octopus_frame();

// Defined below: picks Montserrat for Latin text and the project font for
// anything with Chinese in it.
const lv_font_t *text_font(const char *text);

lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                     uint32_t color, const lv_font_t *font = &radio_font) {
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(object, LV_OPA_COVER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

void set_centered(lv_obj_t *object) {
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
}

const char *playback_text() {
    switch (s_playback_state.load(std::memory_order_acquire)) {
        case RadioPlaybackState::Stopped: return "已暂停";
        case RadioPlaybackState::Connecting: return "连接中";
        case RadioPlaybackState::Buffering: return "缓冲中";
        case RadioPlaybackState::Playing: return "播放中";
        case RadioPlaybackState::Reconnecting: return "重连中";
        case RadioPlaybackState::Error: return "播放失败";
    }
    return "准备播放";
}

// What the header and channel row show. They are painted into the octopus
// overlay by radio_header_paint() whenever this changes or the scene buffers are
// reallocated.
RadioHeaderText s_header;
bool s_overlay_dirty = true;
uint32_t s_overlay_epoch;
uint32_t s_octopus_period = kOctopusFrameMs;
bool s_station_dirty = true;
bool s_location_dirty = true;
bool s_network_dirty = true;
bool s_volume_dirty = true;

void set_header_text(char *dst, std::size_t size, const char *text) {
    if (std::strncmp(dst, text, size) == 0) return;
    std::snprintf(dst, size, "%s", text);
    s_overlay_dirty = true;
}

void refresh_battery() {
    const int soc = bsp_battery_soc();
    if (soc < 0) return;
    // Clamped to 0-100, so "100%" and its terminator are the longest this can
    // be; sized to the header field it is copied into.
    char text[sizeof(s_header.battery)];
    std::snprintf(text, sizeof(text), "%d%%", std::clamp(soc, 0, 100));
    set_header_text(s_header.battery, sizeof(s_header.battery), text);
    if (s_header.battery_low != (soc <= 15)) {
        s_header.battery_low = soc <= 15;
        s_overlay_dirty = true;
    }
}

void apply_network() {
    if (s_header.network == s_network_connected) return;
    s_header.network = s_network_connected;
    s_overlay_dirty = true;
}

void apply_location() {
    if (!s_location) return;
    lv_obj_set_style_text_font(s_location, text_font(s_location_text), 0);
    lv_label_set_text(s_location, s_location_text);
}

// A parsed FM frequency uses the real scale position. Otherwise the needle
// represents the station's position in the current catalog.
int dial_x_for_station() {
    const int span = kDialTrackRight - kDialTrackLeft;
    if (s_station_frequency >= kDialBandLow && s_station_frequency <= kDialBandHigh) {
        const int offset = (s_station_frequency - kDialBandLow) * span /
                           (kDialBandHigh - kDialBandLow);
        return kDialTrackLeft + offset;
    }
    // Inset the positions so the needle never pins to either end.
    const int count = static_cast<int>(std::max<std::size_t>(s_station_count, 1));
    const int index = static_cast<int>(std::min<std::size_t>(s_station_index,
                                                             s_station_count - 1));
    const int inset = span / 8;
    const int usable = span - inset * 2;
    const int offset = count > 1 ? inset + usable * index / (count - 1)
                                 : inset + usable / 2;
    return kDialTrackLeft + offset;
}

void place_needle(int32_t x) {
    s_needle_x = x;
    if (s_dial_needle) {
        lv_obj_set_pos(s_dial_needle, static_cast<int>(x) - kDialNeedleWidth / 2, kNeedleY);
    }
    if (s_dial_glow) {
        lv_obj_set_pos(s_dial_glow, static_cast<int>(x) - 9, kGlowY);
    }
}

void needle_anim_cb(void *, int32_t value) { place_needle(value); }

void apply_dial(bool animate) {
    if (!s_dial_needle || !s_dial_readout) return;
    const bool has_fm =
        s_station_frequency >= kDialBandLow &&
        s_station_frequency <= kDialBandHigh;
    const int target = dial_x_for_station();

    char text[16];
    if (has_fm) {
        std::snprintf(text, sizeof(text), "FM %u.%u",
                      static_cast<unsigned>(s_station_frequency / 10U),
                      static_cast<unsigned>(s_station_frequency % 10U));
    } else {
        std::snprintf(text, sizeof(text), "网络电台");
    }
    lv_label_set_text(s_dial_readout, text);

    lv_anim_delete(s_dial_needle, needle_anim_cb);
    for (lv_obj_t *object : {s_dial_needle, s_dial_glow}) {
        if (!object) continue;
        if (has_fm) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!has_fm) return;
    if (!animate || s_needle_x == target) {
        place_needle(target);
        return;
    }

    // Sweep time scales with distance so a neighbouring station feels quick and
    // a jump across the band still reads as a deliberate tune.
    const int distance = std::abs(target - static_cast<int>(s_needle_x));
    const uint32_t duration =
        static_cast<uint32_t>(std::clamp(160 + distance * 3, 200, 620));

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_dial_needle);
    lv_anim_set_exec_cb(&animation, needle_anim_cb);
    lv_anim_set_values(&animation, s_needle_x, target);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void apply_station() {
    if (!s_station_name || !s_station_description) return;
    char preset[20];
    std::snprintf(preset, sizeof(preset), "CH %02u / %02u",
                  static_cast<unsigned>(s_station_index + 1),
                  static_cast<unsigned>(s_station_count));
    set_header_text(s_header.channel, sizeof(s_header.channel), preset);
    lv_label_set_text(s_station_name, s_station_name_text);
    lv_label_set_text(s_station_description, s_station_description_text);
    apply_dial(true);
    // The octopus owns the rows above this group and writes directly to the
    // panel. Explicitly invalidate the complete lower half as well; relying on
    // per-label invalidation can leave an old station visible when a catalog
    // update lands while the scene is running.
    if (s_main_group) lv_obj_invalidate(s_main_group);
}

void apply_playback() {
    if (!s_status || !s_play_icon) return;
    const RadioPlaybackState state =
        s_playback_state.load(std::memory_order_acquire);
    // The bottom row also carries VOL. Use compact state text there; detailed
    // messages are still kept internally but would overlap the volume column.
    lv_label_set_text(s_status, playback_text());
    const bool playing = state == RadioPlaybackState::Playing ||
                         state == RadioPlaybackState::Buffering ||
                         state == RadioPlaybackState::Connecting ||
                         state == RadioPlaybackState::Reconnecting;
    lv_label_set_text(s_play_icon, playing ? "II" : ">" );
    lv_obj_set_style_text_color(s_play_icon,
                                lv_color_hex(state == RadioPlaybackState::Error
                                                 ? kRed
                                                 : kAmber),
                                0);
}

void apply_volume() {
    char text[16];
    if (s_main_volume) {
        std::snprintf(text, sizeof(text), "%u%%", s_volume);
        lv_label_set_text(s_main_volume, text);
    }
    if (!s_volume_value || !s_volume_bar) return;
    std::snprintf(text, sizeof(text), "%u%%", s_volume);
    lv_label_set_text(s_volume_value, text);
    lv_bar_set_value(s_volume_bar, s_volume, LV_ANIM_ON);
}

void apply_sleep_timer() {
    if (s_page != Page::Main) return;
    const uint32_t seconds = radio_sleep_timer_remaining_seconds();
    if (seconds == s_last_sleep_seconds) return;
    s_last_sleep_seconds = seconds;
    char text[24] = "";
    if (seconds) {
        std::snprintf(text, sizeof(text), "定时 %02u:%02u",
                      static_cast<unsigned>(seconds / 60U),
                      static_cast<unsigned>(seconds % 60U));
    }
    set_header_text(s_header.sleep, sizeof(s_header.sleep), text);
}

void build_main() {
    lv_obj_clean(s_screen);
    s_page = Page::Main;
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_location = nullptr;
    s_main_volume_caption = nullptr;
    s_main_volume = nullptr;
    s_volume_value = nullptr;
    s_volume_bar = nullptr;
    s_pair_dot = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    s_needle_x = kDialNeedleHome;
    s_last_sleep_seconds = UINT32_MAX;

    s_main_group = make_box(s_screen, 0, 0, 240, 320, kBackground);
    s_overlay_dirty = true;

    // Below the scene the water carries on, darkening toward the floor.
    constexpr int kSceneBottom = kOctopusPanelTop + kOctopusPanelHeight;
    lv_obj_t *deep = make_box(s_main_group, 0, kSceneBottom, 240, 320 - kSceneBottom,
                              kDeepWaterTop);
    lv_obj_set_style_bg_grad_color(deep, lv_color_hex(kDeepWaterBottom), 0);
    lv_obj_set_style_bg_grad_dir(deep, LV_GRAD_DIR_VER, 0);

    s_station_name = make_label(s_main_group, "清晨音乐", 12, kStationNameY, 216, kText);
    set_centered(s_station_name);
    lv_obj_set_height(s_station_name, lv_font_get_line_height(&radio_font));
    s_station_description = make_label(s_main_group, "轻松音乐 · 网络直播",
                                       12, kStationDescriptionY, 216, kMuted);
    set_centered(s_station_description);
    lv_obj_set_height(s_station_description, lv_font_get_line_height(&radio_font));

    lv_obj_t *dial = make_box(s_main_group, 13, kDialY, 214, kDialH, kPanel, 8);
    lv_obj_set_style_bg_opa(dial, LV_OPA_80, 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(kGrid), 0);
    lv_obj_set_style_border_width(dial, 1, 0);

    // Tick marks across the 88-108 FM band, with printed anchors.
    constexpr int kTickCount = 41;
    for (int i = 0; i < kTickCount; ++i) {
        const bool major = i % 8 == 0;
        const bool medium = i % 2 == 0;
        if (!major && !medium) continue;
        const int x = kDialTrackLeft +
                      i * (kDialTrackRight - kDialTrackLeft) / (kTickCount - 1);
        const int height = major ? 9 : 5;
        make_box(dial, x, kDialTrackY - height, major ? 2 : 1, height,
                 major ? kGrid : kAmberSoft, 1);
    }
    for (int mhz = 88; mhz <= 108; mhz += 4) {
        const int x = kDialTrackLeft + (mhz - 88) *
                      (kDialTrackRight - kDialTrackLeft) / 20;
        make_label(dial, mhz == 88 ? "88" : mhz == 92 ? "92" : mhz == 96 ? "96"
                       : mhz == 100 ? "100" : mhz == 104 ? "104" : "108",
                   x - 15, 0, 30, kMuted, &lv_font_montserrat_14);
    }
    make_box(dial, kDialTrackLeft, kDialTrackY, kDialTrackRight - kDialTrackLeft + 1, 2,
             kGrid, 1);

    // Soft halo under the needle so the sweep reads clearly against the ticks.
    s_dial_glow = make_box(dial, kDialNeedleHome - 9, kGlowY, 18, 8, kAmber, 4);
    lv_obj_set_style_opa(s_dial_glow, LV_OPA_20, 0);
    s_dial_needle = make_box(dial, kDialNeedleHome - kDialNeedleWidth / 2, kNeedleY,
                             kDialNeedleWidth, kNeedleH, kAmber, 1);

    s_dial_readout = make_label(dial, "", 138, 30, 68, kAmber, &radio_font);
    lv_obj_set_style_text_align(s_dial_readout, LV_TEXT_ALIGN_RIGHT, 0);
    s_location = make_label(dial, s_location_text, 8, 30, 130, kAmber,
                            text_font(s_location_text));

    lv_obj_t *meter_panel = make_box(s_main_group, 13, kMeterY, 214, kMeterH, kPanelSoft, 4);
    lv_obj_set_style_bg_opa(meter_panel, LV_OPA_80, 0);
    s_meter_panel = meter_panel;
    for (std::size_t i = 0; i < kMeterCount; ++i) {
        s_meter[i] = make_box(meter_panel, 9 + static_cast<int>(i) * 11,
                              kMeterH - 4, 6, 2, kAmber, 1);
    }

    s_play_icon = make_label(s_main_group, ">", 13, kStatusY - 1, 24, kAmber,
                             &lv_font_montserrat_20);
    s_status = make_label(s_main_group, "准备播放", 39, kStatusY, 118, kText);
    s_main_volume_caption = make_label(s_main_group, "VOL", 162, kStatusY, 31,
                                       kMuted, &lv_font_montserrat_14);
    s_main_volume = make_label(s_main_group, "", 194, kStatusY, 34, kMuted,
                               &lv_font_montserrat_14);
    lv_label_set_long_mode(s_main_volume_caption, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(s_main_volume, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_main_volume, LV_TEXT_ALIGN_RIGHT, 0);

    apply_network();
    apply_station();
    const uint32_t playback_generation =
        s_playback_generation.load(std::memory_order_acquire);
    apply_playback();
    s_last_playback_generation = playback_generation;
    apply_volume();
    apply_location();
    apply_sleep_timer();
    s_station_dirty = false;
    s_location_dirty = false;
    s_network_dirty = false;
    s_volume_dirty = false;

    // Paint the scene now, before LVGL renders the rebuilt page: from here on
    // the band belongs to the octopus and band_guard_flush keeps LVGL out.
    s_overlay_dirty = true;
    draw_octopus_frame();
}

void build_config(const char *ssid, const char *url) {
    // Leave the main page before clearing, so the full redraw that clearing
    // asks for is not held back from the octopus band.
    s_page = Page::Config;
    lv_obj_clean(s_screen);
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_location = nullptr;
    s_main_volume_caption = nullptr;
    s_main_volume = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    for (auto &bar : s_meter) bar = nullptr;
    s_meter_panel = nullptr;

    make_label(s_screen, "联网收音机", 12, 12, 216, kText, &radio_font_title);
    lv_obj_t *tag = make_label(s_screen, "首次配网", 12, 45, 216, kAmber);
    set_centered(tag);
    s_pair_dot = make_box(s_screen, 112, 77, 16, 16, kRed, LV_RADIUS_CIRCLE);

    lv_obj_t *panel = make_box(s_screen, 18, 111, 204, 132, kPanel, 10);
    lv_obj_set_style_border_color(panel, lv_color_hex(kGrid), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_t *hint = make_label(panel, "手机连接热点", 0, 14, 204, kMuted);
    set_centered(hint);
    lv_obj_t *ssid_label = make_label(panel, ssid && ssid[0] ? ssid : "OCTO Radio",
                                      8, 43, 188, kAmber);
    set_centered(ssid_label);
    hint = make_label(panel, "连接后会自动打开配网页面", 0, 77, 204, kText);
    set_centered(hint);
    lv_obj_t *address = make_label(panel, url && url[0] ? url : "192.168.4.1",
                                   0, 104, 204, kMuted,
                                   &lv_font_montserrat_14);
    set_centered(address);
    hint = make_label(s_screen, "请选择 2.4G 无线网络", 12, 259, 216, kText);
    set_centered(hint);
    hint = make_label(s_screen, "配网完成后会自动开始播放", 12, 286, 216, kMuted);
    set_centered(hint);
}

// Latin text looks better in Montserrat, but only the project font carries the
// Chinese glyphs, so the font follows the string.
const lv_font_t *text_font(const char *text) {
    if (!text) return &lv_font_montserrat_14;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        if (*p >= 0x80) return &radio_font;
    }
    return &lv_font_montserrat_14;
}

void reset_transient_objects(Page page) {
    // Leave the main page before clearing, so the full redraw that clearing
    // asks for is not held back from the octopus band.
    s_page = page;
    lv_obj_clean(s_screen);
    s_station_name = nullptr;
    s_station_description = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_location = nullptr;
    s_main_volume_caption = nullptr;
    s_main_volume = nullptr;
    s_volume_value = nullptr;
    s_volume_bar = nullptr;
    s_pair_dot = nullptr;
    s_dial_needle = nullptr;
    s_dial_glow = nullptr;
    s_dial_readout = nullptr;
    s_wifi_state = nullptr;
    s_wifi_list_hint = nullptr;
    s_wifi_keyboard_network = nullptr;
    s_wifi_password_mask = nullptr;
    s_wifi_keyboard_status = nullptr;
    for (auto &bar : s_meter) bar = nullptr;
    s_meter_panel = nullptr;
    for (auto &row : s_settings_rows) row = nullptr;
    for (auto &label : s_settings_labels) label = nullptr;
    for (auto &box : s_wifi_row_box) box = nullptr;
    for (auto &label : s_wifi_row_ssid) label = nullptr;
    for (auto &label : s_wifi_row_info) label = nullptr;
    for (auto &box : s_wifi_key_box) box = nullptr;
    for (auto &label : s_wifi_key_label) label = nullptr;
}

void apply_settings_selection() {
    for (std::size_t i = 0; i < s_settings_row_count; ++i) {
        if (!s_settings_rows[i] || !s_settings_labels[i]) continue;
        const bool active = i == s_settings_selected;
        lv_obj_set_style_bg_color(s_settings_rows[i],
                                  lv_color_hex(active ? kAmber : kPanel), 0);
        lv_obj_set_style_text_color(s_settings_labels[i],
                                    lv_color_hex(active ? kBackground : kText), 0);
    }
}

void build_settings(RadioSettingsPage page, std::size_t selected, uint8_t volume) {
    reset_transient_objects(Page::Settings);
    s_settings_page = page;
    s_settings_selected = selected;
    s_volume = volume;
    s_settings_row_count = 0;

    if (page == RadioSettingsPage::Volume) {
        lv_obj_t *title = make_label(s_screen, "音量", 12, 16, 216, kText,
                                     &radio_font);
        set_centered(title);
        s_volume_value = make_label(s_screen, "55%", 12, 84, 216, kAmber,
                                    &lv_font_montserrat_20);
        set_centered(s_volume_value);
        s_volume_bar = lv_bar_create(s_screen);
        lv_obj_set_pos(s_volume_bar, 30, 136);
        lv_obj_set_size(s_volume_bar, 180, 14);
        lv_bar_set_range(s_volume_bar, 0, 100);
        lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(kGrid), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(kAmber), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_volume_bar, 7, LV_PART_MAIN);
        lv_obj_set_style_radius(s_volume_bar, 7, LV_PART_INDICATOR);
        lv_obj_t *hint = make_label(s_screen, "上下调节  确定返回",
                                    12, 276, 216, kMuted);
        set_centered(hint);
        apply_volume();
        return;
    }

    static constexpr const char *kMenuItems[] = {
        "音量", "城市电台", "定时播放", "屏幕设置", "网络设置", "返回",
    };
    static constexpr const char *kCityItems[] = {
        "自动定位", "北京", "上海", "广州", "深圳", "长沙", "成都", "寻找城市",
    };
    static constexpr const char *kSleepTimerItems[] = {
        "无定时", "15 分钟", "30 分钟", "60 分钟", "90 分钟", "返回",
    };
    char brightness_item[32];
    char timeout_item[48];
    std::snprintf(brightness_item, sizeof(brightness_item), "亮度 %u%%",
                  static_cast<unsigned>(radio_display_brightness()));
    const uint16_t timeout = radio_display_timeout_seconds();
    if (!timeout) {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 关闭");
    } else if (timeout < 60) {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 秒",
                      static_cast<unsigned>(timeout));
    } else {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 分钟",
                      static_cast<unsigned>(timeout / 60));
    }
    const char *display_items[] = {brightness_item, timeout_item, "返回"};
    static constexpr const char *kWifiMenuItems[] = {
        "连接新网络", "已存网络", "返回",
    };

    const char *const *items = nullptr;
    std::size_t item_count = 0;
    const char *title_text = "设置";
    const char *hint_text = "上下选择  确定进入";
    switch (page) {
        case RadioSettingsPage::Menu:
            items = kMenuItems;
            item_count = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
            break;
        case RadioSettingsPage::City:
            items = kCityItems;
            item_count = sizeof(kCityItems) / sizeof(kCityItems[0]);
            title_text = "城市电台";
            hint_text = "确定选择  长按返回";
            break;
        case RadioSettingsPage::SleepTimer:
            items = kSleepTimerItems;
            item_count = sizeof(kSleepTimerItems) / sizeof(kSleepTimerItems[0]);
            title_text = "定时播放";
            hint_text = "确定开始  长按返回";
            break;
        case RadioSettingsPage::Display:
            items = display_items;
            item_count = sizeof(display_items) / sizeof(display_items[0]);
            title_text = "屏幕设置";
            hint_text = "确定切换  长按返回";
            break;
        case RadioSettingsPage::WifiMenu:
            items = kWifiMenuItems;
            item_count = sizeof(kWifiMenuItems) / sizeof(kWifiMenuItems[0]);
            title_text = "网络设置";
            hint_text = "确定进入  长按返回";
            break;
        default:
            return;
    }

    const bool compact = item_count > 3;
    const bool extra_compact = item_count > 6;
    const bool city_rows = item_count >= 8;
    const int first_y = city_rows ? 34
                                  : (extra_compact ? 39 : (compact ? 45 : 70));
    const int row_height = city_rows ? 27
                                     : (extra_compact ? 29 : (compact ? 32 : 42));
    const int row_gap = city_rows ? 30
                                  : (extra_compact ? 34 : (compact ? 38 : 55));
    static_assert(kSettingsMaxRows >= 8, "settings rows must hold the city list");
    if (item_count > kSettingsMaxRows) item_count = kSettingsMaxRows;
    s_settings_row_count = item_count;
    if (s_settings_selected >= item_count) s_settings_selected = 0;

    lv_obj_t *title = make_label(s_screen, title_text, 12, 13, 216, kText,
                                 &radio_font);
    set_centered(title);
    for (std::size_t i = 0; i < item_count; ++i) {
        const int y = first_y + static_cast<int>(i) * row_gap;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, row_height, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], items[i], 0,
                                          city_rows ? 5 :
                                          extra_compact ? 5 : (compact ? 7 : 12),
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    lv_obj_t *hint = make_label(s_screen, hint_text, 12, 283, 216, kMuted);
    set_centered(hint);
}

void candidate_city_name(const char *name, char *out, std::size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = '\0';
    if (!name) return;
    std::snprintf(out, capacity, "%s", name);

    std::size_t length = std::strlen(out);
    std::size_t body_length = length;
    const char *const autonomous_suffix = "自治州";
    const std::size_t autonomous_length = std::strlen(autonomous_suffix);
    if (length > autonomous_length &&
        std::strcmp(out + length - autonomous_length, autonomous_suffix) == 0) {
        body_length -= autonomous_length;
        // Keep the familiar place name and omit the long ethnic descriptor.
        static constexpr const char *kEthnicNames[] = {
            "柯尔克孜", "哈萨克", "维吾尔", "蒙古", "朝鲜", "布依",
            "景颇",   "傈僳",   "哈尼",   "土家", "藏",   "羌",
            "彝",     "壮",     "侗",     "苗",   "白",   "傣", "回",
        };
        for (const char *ethnic : kEthnicNames) {
            const char *found = std::strstr(out, ethnic);
            if (found && static_cast<std::size_t>(found - out) < body_length) {
                body_length = static_cast<std::size_t>(found - out);
            }
        }
    } else {
        for (const char *suffix : {"地区", "盟"}) {
            const std::size_t suffix_length = std::strlen(suffix);
            if (length > suffix_length &&
                std::strcmp(out + length - suffix_length, suffix) == 0) {
                body_length = length - suffix_length;
                break;
            }
        }
    }
    out[body_length] = '\0';
}

void build_city_matches(const char *const *cities, const char *const *provinces,
                        std::size_t count, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::CityMatches;
    count = std::min(count, kSettingsMaxRows);
    selected = count ? std::min(selected, count - 1) : 0;
    s_settings_selected = selected;
    s_settings_row_count = count;

    lv_obj_t *title = make_label(s_screen, "选择城市", 12, 12, 216, kText, &radio_font);
    set_centered(title);
    char state_text[32] = {};
    std::snprintf(state_text, sizeof(state_text), "找到 %u 个城市",
                  static_cast<unsigned>(count));
    lv_obj_t *state = make_label(s_screen, state_text, 12, 38, 216, kMuted,
                                 &radio_font);
    set_centered(state);

    for (std::size_t i = 0; i < count; ++i) {
        char text[128] = {};
        char short_name[64] = {};
        candidate_city_name(cities[i], short_name, sizeof(short_name));
        std::snprintf(text, sizeof(text), "%s  %s",
                      short_name,
                      provinces[i] ? provinces[i] : "");
        const int y = 64 + static_cast<int>(i) * 27;
        s_settings_rows[i] = make_box(s_screen, 20, y, 200, 24, kPanel, 6);
        s_settings_labels[i] = make_label(s_settings_rows[i], text, 0, 3, 200,
                                          kText, &radio_font);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    lv_obj_t *hint = make_label(s_screen, "上下选择  长按返回", 12, 286, 216, kMuted);
    set_centered(hint);
}

void build_saved_wifi(const char *const *ssids, std::size_t count,
                      std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::SavedWifi;
    const std::size_t total = count + 1;
    selected = std::min(selected, total - 1);

    lv_obj_t *title = make_label(s_screen, "已存网络", 12, 12, 216, kText,
                                 &radio_font);
    set_centered(title);
    lv_obj_t *state = make_label(s_screen,
                                 count ? "上下选择  确定设置" : "无保存网络",
                                 12, 40, 216, count ? kMuted : kAmber);
    set_centered(state);

    const std::size_t first = total <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             total - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        if (index >= total) break;
        const bool active = index == selected;
        const int y = 67 + static_cast<int>(row) * 34;
        s_settings_rows[row] = make_box(s_screen, 18, y, 204, 29,
                                        active ? kAmberSoft : kPanel, 5);
        const char *text = index < count ? ssids[index] : "返回";
        s_settings_labels[row] = make_label(
            s_settings_rows[row], text, 8, 6, 188, active ? kText : kMuted,
            index < count ? text_font(text) : &radio_font);
        set_centered(s_settings_labels[row]);
    }
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 286, 216, kMuted);
    set_centered(hint);
}

void build_wifi_action(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::WifiAction;
    s_settings_selected = selected % 3;
    s_settings_row_count = 3;
    lv_obj_t *title = make_label(s_screen, "网络设置", 12, 13, 216, kText,
                                 &radio_font);
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 45, 216,
                                   kAmber, text_font(ssid));
    set_centered(network);
    static constexpr const char *kItems[] = {"设为首选", "删除网络", "返回"};
    for (std::size_t i = 0; i < 3; ++i) {
        const int y = 83 + static_cast<int>(i) * 55;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 42, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 12,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 283, 216, kMuted);
    set_centered(hint);
}

void build_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = RadioSettingsPage::WifiDeleteConfirm;
    s_settings_selected = selected % 2;
    s_settings_row_count = 2;
    lv_obj_t *title = make_label(s_screen, "确认删除", 12, 18, 216, kText,
                                 &radio_font);
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 55, 216,
                                   kAmber, text_font(ssid));
    set_centered(network);
    lv_obj_t *warning = make_label(s_screen, "删除后无法自动连接", 12, 88, 216,
                                   kMuted);
    set_centered(warning);
    static constexpr const char *kItems[] = {"返回", "确认删除"};
    for (std::size_t i = 0; i < 2; ++i) {
        const int y = 132 + static_cast<int>(i) * 61;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 45, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 13,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
}

void update_wifi_list(const WifiNetworkView *networks, std::size_t count,
                      std::size_t selected, bool scanning, bool can_cancel) {
    const char *state_text = scanning ? "正在寻找网络" :
                             count ? "上下选择  确定" :
                             can_cancel ? "等待网络  长按返回"
                                        : "等待网络  长按重试";
    lv_label_set_text(s_wifi_state, state_text);
    lv_obj_set_style_text_color(s_wifi_state,
                                lv_color_hex(scanning ? kAmber : kMuted), 0);
    if (s_wifi_list_hint) {
        lv_label_set_text(s_wifi_list_hint,
                          can_cancel ? "确定设置  长按返回" : "确定后设置网络");
    }

    const std::size_t first = count <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             count - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        const bool present = index < count;
        const bool active = present && index == selected;
        lv_obj_set_style_bg_color(s_wifi_row_box[row],
                                  lv_color_hex(active ? kAmberSoft : kPanel), 0);
        if (!present) {
            lv_label_set_text(s_wifi_row_ssid[row], "");
            lv_label_set_text(s_wifi_row_info[row], "");
            continue;
        }

        const uint32_t color = active ? kText : kMuted;
        lv_label_set_text(s_wifi_row_ssid[row], networks[index].ssid);
        lv_obj_set_style_text_font(s_wifi_row_ssid[row],
                                   text_font(networks[index].ssid), 0);
        lv_obj_set_style_text_color(s_wifi_row_ssid[row], lv_color_hex(color), 0);
        char info[16];
        std::snprintf(info, sizeof(info), "%s %ddB",
                      networks[index].secure ? "*" : "OPEN",
                      static_cast<int>(networks[index].rssi));
        lv_label_set_text(s_wifi_row_info[row], info);
        lv_obj_set_style_text_color(s_wifi_row_info[row],
                                    lv_color_hex(active ? kAmber : kMuted), 0);
    }
}

void build_wifi_list(const WifiNetworkView *networks, std::size_t count,
                     std::size_t selected, bool scanning, const char *setup_ap_ssid,
                     bool can_cancel) {
    reset_transient_objects(Page::WifiList);
    // The compact UI font contains the complete setup vocabulary. The larger
    // station-title font intentionally contains only station-name glyphs.
    make_label(s_screen, "选择无线网络", 12, 12, 216, kText, &radio_font);
    s_wifi_state = make_label(s_screen, "", 12, 42, 216, kMuted);
    set_centered(s_wifi_state);

    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const int y = 68 + static_cast<int>(row) * 34;
        s_wifi_row_box[row] = make_box(s_screen, 12, y, 216, 29, kPanel, 5);
        s_wifi_row_ssid[row] = make_label(s_wifi_row_box[row], "", 9, 6, 154,
                                          kMuted, &lv_font_montserrat_14);
        s_wifi_row_info[row] = make_label(s_wifi_row_box[row], "", 158, 7, 50,
                                          kMuted, &lv_font_montserrat_14);
        lv_obj_set_style_text_align(s_wifi_row_info[row], LV_TEXT_ALIGN_RIGHT, 0);
    }

    make_box(s_screen, 12, 277, 216, 1, kGrid);
    s_wifi_list_hint = make_label(s_screen, "", 12, 282, 216, kText);
    set_centered(s_wifi_list_hint);
    char portal[64];
    std::snprintf(portal, sizeof(portal), "WEB: %s",
                  setup_ap_ssid && setup_ap_ssid[0] ? setup_ap_ssid : "OCTO Radio");
    lv_obj_t *hint = make_label(s_screen, portal, 12, 302, 216, kMuted,
                                &lv_font_montserrat_14);
    set_centered(hint);
    update_wifi_list(networks, count, selected, scanning, can_cancel);
}

// The Wi-Fi password keyboard and the station search keyboard are one layout.
// They differ in the title, what the box above the keys shows (the password as
// stars, the search text as typed) and the hint under the keys.
enum class KeyboardUse { WifiPassword, StationSearch };

void update_keyboard(KeyboardUse use, const char *context, const char *text,
                     WifiKeyboardPage page, std::size_t selected, const char *status) {
    lv_label_set_text(s_wifi_keyboard_network, context ? context : "");
    lv_obj_set_style_text_font(s_wifi_keyboard_network, text_font(context), 0);

    char shown[48] = {};
    const std::size_t length = text ? std::strlen(text) : 0;
    if (use == KeyboardUse::WifiPassword) {
        const std::size_t stars = std::min<std::size_t>(length, 38);
        std::memset(shown, '*', stars);
        shown[stars] = '\0';
    } else {
        // Latin text only: the tail is shown when it is longer than the box.
        const std::size_t keep = std::min<std::size_t>(length, 24);
        std::memcpy(shown, text + (length - keep), keep);
        shown[keep] = '\0';
    }
    const char *placeholder = use == KeyboardUse::WifiPassword ? "PASS" : "_";
    lv_label_set_text(s_wifi_password_mask, length ? shown : placeholder);
    lv_obj_set_style_text_color(
        s_wifi_password_mask,
        lv_color_hex(length ? (use == KeyboardUse::WifiPassword ? kText : kAmber) : kMuted), 0);

    const std::size_t key_count = radio_ui_wifi_key_count(page);
    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        if (i >= key_count) {
            lv_obj_add_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
        const bool active = i == selected;
        lv_obj_set_style_bg_color(s_wifi_key_box[i],
                                  lv_color_hex(active ? kAmber : kPanelSoft), 0);
        lv_label_set_text(s_wifi_key_label[i], radio_ui_wifi_key(page, i));
        lv_obj_set_style_text_color(s_wifi_key_label[i],
                                    lv_color_hex(active ? kBackground : kText), 0);
    }

    lv_label_set_text(s_wifi_keyboard_status,
                      status && status[0] ? status : "上下选择  确定");
    lv_obj_set_style_text_color(s_wifi_keyboard_status,
                                lv_color_hex(status && status[0] ? kRed : kMuted), 0);
}

void build_keyboard(KeyboardUse use, const char *context, const char *text,
                    WifiKeyboardPage page, std::size_t selected, const char *status) {
    reset_transient_objects(use == KeyboardUse::WifiPassword ? Page::WifiKeyboard
                                                              : Page::SearchKeyboard);
    make_label(s_screen, use == KeyboardUse::WifiPassword ? "网络设置" : "寻找城市", 12, 10,
               216, kText, &radio_font);
    s_wifi_keyboard_network = make_label(s_screen, "", 12, 39, 216, kAmber,
                                          &lv_font_montserrat_14);
    set_centered(s_wifi_keyboard_network);

    lv_obj_t *password_box = make_box(s_screen, 18, 66, 204, 34, kPanel, 6);
    s_wifi_password_mask = make_label(password_box, "", 8, 8, 188, kMuted,
                                      &lv_font_montserrat_14);
    set_centered(s_wifi_password_mask);

    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        constexpr int kColumns = 6;
        const int x = 15 + static_cast<int>(i % kColumns) * 35;
        const int y = 109 + static_cast<int>(i / kColumns) * 28;
        // One styled label per key keeps the keyboard within the 24 KB LVGL
        // heap. A separate box + label for every key exhausted that heap near
        // key 31 and caused an immediate Store access fault.
        s_wifi_key_label[i] = make_label(s_screen, "", x, y, 31, kText,
                                         &lv_font_montserrat_14);
        s_wifi_key_box[i] = s_wifi_key_label[i];
        lv_obj_set_size(s_wifi_key_label[i], 31, 24);
        lv_obj_set_style_bg_color(s_wifi_key_label[i], lv_color_hex(kPanelSoft), 0);
        lv_obj_set_style_bg_opa(s_wifi_key_label[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_wifi_key_label[i], 4, 0);
        lv_obj_set_style_pad_top(s_wifi_key_label[i], 4, 0);
        set_centered(s_wifi_key_label[i]);
    }

    make_box(s_screen, 12, 281, 216, 1, kGrid);
    s_wifi_keyboard_status = make_label(s_screen, "", 12, 287, 216, kMuted);
    set_centered(s_wifi_keyboard_status);
    lv_obj_t *hint = make_label(
        s_screen, use == KeyboardUse::WifiPassword ? "长按上下调节" : "GO 开始寻找", 12, 304,
        216, kMuted);
    set_centered(hint);
    update_keyboard(use, context, text, page, selected, status);
}

void build_wifi_connecting(const char *ssid, const char *detail) {
    reset_transient_objects(Page::WifiConnecting);
    make_label(s_screen, "正在连接无线网络", 12, 37, 216, kText,
               &radio_font);
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 102, 216,
                                   kAmber, text_font(ssid));
    set_centered(network);
    s_pair_dot = make_box(s_screen, 108, 151, 24, 24, kAmber, LV_RADIUS_CIRCLE);
    lv_obj_t *message = make_label(s_screen, detail ? detail : "正在连接",
                                   12, 205, 216, kMuted);
    set_centered(message);
}

void timer_callback(lv_timer_t *) {
    s_animation += 17;
    if (s_page == Page::Config || s_page == Page::WifiConnecting) {
        if (s_pair_dot) {
            const uint8_t triangle = s_animation < 128 ? s_animation : 255 - s_animation;
            lv_obj_set_style_opa(s_pair_dot, 70 + triangle, 0);
        }
        return;
    }

    if (s_page == Page::Main) {
        // Setters can run from the player, catalog and Wi-Fi tasks. Applying
        // the complete state here, from LVGL's own timer, guarantees a station
        // change cannot be lost at a page transition or during a missed lock.
        if (s_network_dirty) {
            apply_network();
            s_network_dirty = false;
        }
        if (s_station_dirty) {
            apply_station();
            s_station_dirty = false;
        }
        const uint32_t playback_generation =
            s_playback_generation.load(std::memory_order_acquire);
        if (playback_generation != s_last_playback_generation) {
            s_last_playback_generation = playback_generation;
            apply_playback();
        }
        if (s_volume_dirty) {
            apply_volume();
            s_volume_dirty = false;
        }
        if (s_location_dirty) {
            apply_location();
            s_location_dirty = false;
        }
    }
    if (++s_clock_ticks >= 20) {
        s_clock_ticks = 0;
        if (s_page == Page::Main) {
            const std::time_t now = std::time(nullptr);
            if (now > 1700000000) {
                std::tm local_time = {};
                localtime_r(&now, &local_time);
                char text[8];
                std::snprintf(text, sizeof(text), "%02d:%02d", local_time.tm_hour,
                              local_time.tm_min);
                set_header_text(s_header.clock, sizeof(s_header.clock), text);
            }
        }
        apply_sleep_timer();
    }

    if (++s_battery_ticks >= 300) {
        s_battery_ticks = 0;
        refresh_battery();
    }

    const uint32_t generation = s_level_generation.load(std::memory_order_acquire);
    if (generation != s_last_level_generation) {
        s_last_level_generation = generation;
        // Each bar's move and resize adds its own invalid areas: 18 bars give
        // more than LVGL's 32-entry buffer holds, and on overflow LVGL redraws
        // the whole screen, which blanks the octopus band until its next
        // frame. Invalidating the panel first makes every bar area fall
        // inside it, so LVGL keeps the one area and drops the rest.
        if (s_meter_panel) lv_obj_invalidate(s_meter_panel);
        for (std::size_t i = 0; i < kMeterCount; ++i) {
            if (!s_meter[i]) continue;
            const int height = 2 + s_levels[i] * 9 / 100;
            lv_obj_set_pos(s_meter[i], 9 + static_cast<int>(i) * 11, kMeterH - 2 - height);
            lv_obj_set_size(s_meter[i], 6, height);
            const uint32_t color = s_levels[i] > 85 ? kRed :
                                   s_levels[i] > 62 ? kAmber : kGreen;
            lv_obj_set_style_bg_color(s_meter[i], lv_color_hex(color), 0);
        }
    }
}

// ── header overlay ───────────────────────────────────────

// Decodes UTF-8 into code points; returns how many were written.
// LVGL only holds a background colour in the octopus band, so while the scene
// is on the panel none of those rows may reach it: they show as a black flash
// until the next scene frame. Clipping invalid areas cannot guarantee that
// (LVGL falls back to a full-screen redraw without telling anyone when its
// area list overflows, and it probes the rounder with fake areas), so the band
// is cut out of every flush instead. LVGL still renders those rows now and
// then; they just never leave the draw buffer.
lv_display_flush_cb_t s_panel_flush;

void band_guard_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    constexpr int32_t kBandBottom = kOctopusPanelTop + kOctopusPanelHeight;
    if (s_page == Page::Main && radio_octopus_active() && area->y1 < kBandBottom) {
        if (area->y2 < kBandBottom) {
            lv_display_flush_ready(display);
            return;
        }
        const uint32_t stride = lv_draw_buf_width_to_stride(lv_area_get_width(area),
                                                            lv_display_get_color_format(display));
        lv_area_t rest = *area;
        rest.y1 = kBandBottom;
        s_panel_flush(display, &rest, pixels + static_cast<size_t>(kBandBottom - area->y1) * stride);
        return;
    }
    s_panel_flush(display, area, pixels);
}

// Runs in the LVGL task. Screen blanking pauses scene time but deliberately
// retains the panel buffers: releasing them can make wake-time reallocation
// fail, leaving the band unguarded when a station change redraws LVGL. Off the
// main page, drawing is skipped but the buffers remain available for return.
// The timer's period follows measured frame time so the scene never holds the
// LVGL task for more than about 45% of the time, whatever the device manages.
// Renders one scene frame, repainting the overlay when it has changed. Returns
// false while the buffers are unavailable. Call with the LVGL lock held: the
// scene goes straight to the panel, the same way LVGL's own flush does.
bool draw_octopus_frame() {
    OctopusOverlay *overlay = radio_octopus_acquire();
    if (!overlay) return false;
    if (s_overlay_dirty || s_overlay_epoch != radio_octopus_epoch()) {
        radio_header_paint(overlay, s_header);
        s_overlay_dirty = false;
        s_overlay_epoch = radio_octopus_epoch();
    }
    return radio_octopus_draw_frame(
        s_playback_state.load(std::memory_order_acquire) ==
        RadioPlaybackState::Playing);
}

void octopus_timer_callback(lv_timer_t *timer) {
    if (!radio_display_screen_on()) {
        radio_octopus_pause();
        return;
    }
    if (s_page != Page::Main) return;
    if (!draw_octopus_frame()) return;

    const uint32_t frame_ms = radio_octopus_last_frame_us() / 1000;
    const uint32_t period = std::clamp<uint32_t>(frame_ms * 9 / 4, kOctopusFrameMs, 300);
    if (period != s_octopus_period) {
        s_octopus_period = period;
        lv_timer_set_period(timer, period);
    }
}

// Takes the LVGL lock, warning when it times out. A silent failure here looks
// exactly like a frozen screen from the outside, so it must be visible in the
// log. Call sites keep their own timeout: the long ones rebuild a page, the
// short ones only refresh a value and can safely skip a round.
bool ui_lock(int timeout_ms, const char *who) {
    if (bsp_lvgl_lock(timeout_ms)) return true;
    ESP_LOGW("radio_ui", "LVGL lock timed out after %d ms in %s; skipping the update",
             timeout_ms, who);
    return false;
}

}  // namespace

bool radio_ui_init() {
    if (!ui_lock(1000, "radio_ui_init")) return false;
    // The battery gauge is ready before the UI starts. Read it once now so the
    // first visible frame already carries the percentage instead of "--%".
    refresh_battery();
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    build_main();
    lv_display_t *display = lv_display_get_default();
    if (!s_panel_flush) {
        s_panel_flush = display->flush_cb;
        lv_display_set_flush_cb(display, band_guard_flush);
    }
    lv_screen_load(s_screen);
    // Push the complete first page while the backlight is still off. The octopus
    // band is already on the panel; this flush supplies the lower UI rows.
    lv_refr_now(display);
    lv_timer_create(timer_callback, 50, nullptr);
    lv_timer_create(octopus_timer_callback, kOctopusFrameMs, nullptr);
    bsp_lvgl_unlock();
    return true;
}

void radio_ui_show_main() {
    if (!s_screen || !ui_lock(500, "radio_ui_show_main")) return;
    build_main();
    bsp_lvgl_unlock();
}

void radio_ui_refresh_now() {
    if (!s_screen || !ui_lock(500, "radio_ui_refresh_now")) return;
    if (lv_display_t *display = lv_display_get_default()) {
        lv_refr_now(display);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_config(const char *ssid, const char *url) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_config")) return;
    build_config(ssid, url);
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_list(const WifiNetworkView *networks, std::size_t count,
                             std::size_t selected, bool scanning,
                             const char *setup_ap_ssid, bool can_cancel) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_wifi_list")) return;
    if (s_page == Page::WifiList && s_wifi_state) {
        update_wifi_list(networks, count, selected, scanning, can_cancel);
    } else {
        build_wifi_list(networks, count, selected, scanning, setup_ap_ssid,
                        can_cancel);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_keyboard(const char *ssid, std::size_t password_length,
                                 WifiKeyboardPage page, std::size_t selected,
                                 const char *status) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_wifi_keyboard")) return;
    // The keyboard shows stars, so only the length is needed.
    char stars[64] = {};
    std::memset(stars, '*', std::min<std::size_t>(password_length, sizeof(stars) - 1));
    const char *context = ssid ? ssid : "WIFI";
    if (s_page == Page::WifiKeyboard && s_wifi_password_mask) {
        update_keyboard(KeyboardUse::WifiPassword, context, stars, page, selected, status);
    } else {
        build_keyboard(KeyboardUse::WifiPassword, context, stars, page, selected, status);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_search_keyboard(const char *query, WifiKeyboardPage page,
                                   std::size_t selected, const char *status) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_search_keyboard")) return;
    if (s_page == Page::SearchKeyboard && s_wifi_password_mask) {
        update_keyboard(KeyboardUse::StationSearch, "BJ SH GZ SZ", query, page, selected, status);
    } else {
        build_keyboard(KeyboardUse::StationSearch, "BJ SH GZ SZ", query, page, selected, status);
    }
    bsp_lvgl_unlock();
}

bool radio_ui_can_render(const char *text) {
    if (!text) return true;
    for (const char *p = text; *p;) {
        uint32_t cp = static_cast<unsigned char>(*p);
        int extra = 0;
        if (cp >= 0xf0) { cp &= 0x07; extra = 3; }
        else if (cp >= 0xe0) { cp &= 0x0f; extra = 2; }
        else if (cp >= 0xc0) { cp &= 0x1f; extra = 1; }
        ++p;
        for (; extra > 0 && (*p & 0xc0) == 0x80; --extra) cp = (cp << 6) | (*p++ & 0x3f);
        if (cp < 0x80) continue;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(&radio_font, &g, cp, 0)) return false;
    }
    return true;
}

void radio_ui_show_wifi_connecting(const char *ssid, const char *detail) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_wifi_connecting")) return;
    build_wifi_connecting(ssid, detail);
    bsp_lvgl_unlock();
}

const char *radio_ui_wifi_key(WifiKeyboardPage page, std::size_t index) {
    switch (page) {
        case WifiKeyboardPage::Lower:
            return index < sizeof(kLowerKeys) / sizeof(kLowerKeys[0])
                       ? kLowerKeys[index]
                       : "";
        case WifiKeyboardPage::Upper:
            return index < sizeof(kUpperKeys) / sizeof(kUpperKeys[0])
                       ? kUpperKeys[index]
                       : "";
        case WifiKeyboardPage::Symbols:
            return index < sizeof(kSymbolKeys) / sizeof(kSymbolKeys[0])
                       ? kSymbolKeys[index]
                       : "";
    }
    return "";
}

std::size_t radio_ui_wifi_key_count(WifiKeyboardPage page) {
    switch (page) {
        case WifiKeyboardPage::Lower: return sizeof(kLowerKeys) / sizeof(kLowerKeys[0]);
        case WifiKeyboardPage::Upper: return sizeof(kUpperKeys) / sizeof(kUpperKeys[0]);
        case WifiKeyboardPage::Symbols:
            return sizeof(kSymbolKeys) / sizeof(kSymbolKeys[0]);
    }
    return 0;
}

void radio_ui_set_network(bool connected, const char *detail) {
    s_network_connected = connected;
    s_network_dirty = true;
    if (detail) std::snprintf(s_network_detail, sizeof(s_network_detail), "%s", detail);
    if (!s_screen || !ui_lock(250, "radio_ui_set_network")) return;
    apply_network();
    bsp_lvgl_unlock();
}

void radio_ui_set_location(const char *location) {
    std::snprintf(s_location_text, sizeof(s_location_text), "%s",
                  location && location[0] ? location : "中国");
    s_location_dirty = true;
    if (!s_screen || !ui_lock(250, "radio_ui_set_location")) return;
    apply_location();
    bsp_lvgl_unlock();
}

void radio_ui_set_station(std::size_t index, std::size_t count,
                          const RadioStation &station) {
    s_station_index = index;
    s_station_count = std::max<std::size_t>(count, 1);
    s_station_frequency = station.frequency_decihz;
    std::snprintf(s_station_name_text, sizeof(s_station_name_text), "%s", station.name);
    std::snprintf(s_station_description_text, sizeof(s_station_description_text), "%s",
                  station.description);
    s_station_dirty = true;
    ESP_LOGI("radio_ui", "set station page=%d name='%s' count=%u label=%d/%d",
             static_cast<int>(s_page), s_station_name_text,
             static_cast<unsigned>(s_station_count), s_station_name != nullptr,
             s_station_description != nullptr);
    if (!s_screen || !ui_lock(250, "radio_ui_set_station")) return;
    apply_station();
    bsp_lvgl_unlock();
}

void radio_ui_set_playback(RadioPlaybackState state, const char *detail) {
    // This is called from the real-time playback task. Publish the state and
    // let the LVGL timer paint it; waiting on the LVGL mutex here can starve
    // the I2S DMA and create an audible gap.
    (void)detail;
    s_playback_state.store(state, std::memory_order_release);
    s_playback_generation.fetch_add(1, std::memory_order_release);
}

void radio_ui_set_audio_levels(const uint8_t *levels, std::size_t count) {
    if (!levels) return;
    const std::size_t copy_count = std::min(count, kMeterCount);
    std::memcpy(s_levels, levels, copy_count);
    if (copy_count < kMeterCount) {
        std::memset(s_levels + copy_count, 0, kMeterCount - copy_count);
    }
    s_level_generation.fetch_add(1, std::memory_order_release);
}

void radio_ui_show_settings(RadioSettingsPage page, std::size_t selected,
                            uint8_t volume) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_settings")) return;
    if (s_page == Page::Settings && s_settings_page == page &&
        page != RadioSettingsPage::Volume &&
        page != RadioSettingsPage::Display && s_settings_row_count > 0) {
        s_settings_selected = selected % s_settings_row_count;
        apply_settings_selection();
    } else {
        build_settings(page, selected, volume);
    }
    bsp_lvgl_unlock();
}

void radio_ui_show_city_matches(const char *const *cities,
                                const char *const *provinces,
                                std::size_t count, std::size_t selected) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_city_matches")) return;
    build_city_matches(cities, provinces, count, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_saved_wifi(const char *const *ssids, std::size_t count,
                              std::size_t selected) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_saved_wifi")) return;
    build_saved_wifi(ssids, count, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_action(const char *ssid, std::size_t selected) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_wifi_action")) return;
    build_wifi_action(ssid, selected);
    bsp_lvgl_unlock();
}

void radio_ui_show_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    if (!s_screen || !ui_lock(500, "radio_ui_show_wifi_delete_confirm")) return;
    build_wifi_delete_confirm(ssid, selected);
    bsp_lvgl_unlock();
}

void radio_ui_set_volume(uint8_t volume) {
    s_volume = volume;
    s_volume_dirty = true;
    if (!s_screen || !ui_lock(250, "radio_ui_set_volume")) return;
    apply_volume();
    bsp_lvgl_unlock();
}
