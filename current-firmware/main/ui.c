#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "sdkconfig.h"
#include "board.h"
#include "camera.h"
#include "odin.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "net.h"

#define C_BG    0x04080f
#define C_PANEL 0x07121f
#define C_LINE  0x0e3a52
#define C_CY    0x19e3ff
#define C_MG    0xb44dff
#define C_TX    0xcfefff
#define C_DIM   0x5f8ba0
#define C_OK    0x2dff9a
#define C_WARN  0xffb020
#define C_ERR   0xff4d5e
#define C_SEL   0x0a2a3d

#define LOCK()   lvgl_port_lock(0)
#define UNLOCK() lvgl_port_unlock()

#define WAVE_W   636
#define WAVE_H   170
#define WAVE_N   80

#define F12 (&lv_font_montserrat_12)
#define F14 (&lv_font_montserrat_14)
#define F16 (&lv_font_montserrat_16)
#define F20 (&lv_font_montserrat_20)
#define F24 (&lv_font_montserrat_24)
#define F30 (&lv_font_montserrat_30)
#define F42 (&lv_font_montserrat_42)
#define F48 (&lv_font_montserrat_48)

static lv_obj_t *pg[3];            /* 0 home, 1 camera, 2 settings */
static lv_obj_t *nav[3];
static lv_obj_t *talk_btn, *talk_lbl;
static lv_obj_t *lb_time, *lb_sec, *lb_date, *lb_temp, *lb_wdesc, *lb_feels, *lb_wind, *lb_hum, *lb_ip;
static lv_obj_t *lb_wifi, *led_llm, *lb_llm, *lb_model, *lb_eco, *lb_bat;
static lv_obj_t *lb_state, *lb_query, *lb_resp, *resp_cont;
static lv_obj_t *wave[3];
static lv_obj_t *lb_vol, *lb_bri, *sl_vol, *sl_bri, *sw_pow, *dd_model, *ta_tts, *kb, *led_tts, *lb_tts;
static char s_model_opts[1280];
static lv_obj_t *s_web_credentials;
static lv_obj_t *btn_res, *sw_aud, *lb_camtag;
/* Higher capture resolutions are selectable only through the web portal. */
#define BOARD_CAMERA_RES_COUNT 3
static lv_obj_t *s_camera_image, *s_camera_message;
static uint16_t *s_camera_pixels;
static lv_image_dsc_t s_camera_dsc;
static bool s_camera_live;
static bool s_camera_unavailable;
static lv_obj_t *s_weather_location, *s_web_address;

static lv_style_t st_btn, st_btn_on, st_talk_on;
static lv_point_precise_t wave_pts[3][WAVE_N + 1];
static float s_amp = 0.1f, s_t;
static odin_state_t s_state = ODIN_STANDBY;

static char s_resp[512];
static char s_shown[512];
static size_t s_resp_len, s_resp_pos;
static lv_timer_t *s_type_timer;

/* ================================================================ helpers */
static lv_obj_t *mk_panel(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_clear(lv_obj_t *p)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *p, const char *txt, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}

static lv_obj_t *mk_cap(lv_obj_t *p, const char *txt)
{
    lv_obj_t *l = mk_label(p, txt, F12, C_DIM);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    return l;
}

static void set_col(lv_obj_t *l, uint32_t col) { lv_obj_set_style_text_color(l, lv_color_hex(col), 0); }

static lv_obj_t *mk_kv(lv_obj_t *p, const char *key, lv_obj_t **val)
{
    lv_obj_t *row = mk_clear(p);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mk_label(row, key, F14, C_DIM);
    *val = mk_label(row, "--", F14, C_TX);
    return row;
}

static void style_slider(lv_obj_t *s)
{
    lv_obj_set_size(s, LV_PCT(100), 14);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_CY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_CY), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 9, LV_PART_KNOB);
}

static void style_switch(lv_obj_t *sw)
{
    lv_obj_set_size(sw, 66, 34);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_CY), (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_DIM), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xffffff), (lv_style_selector_t)LV_PART_KNOB | LV_STATE_CHECKED);
}

static void style_btnm(lv_obj_t *b)
{
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(b, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PANEL), LV_PART_ITEMS);
    lv_obj_set_style_border_width(b, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), LV_PART_ITEMS);
    lv_obj_set_style_radius(b, 8, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(b, lv_color_hex(C_DIM), LV_PART_ITEMS);
    lv_obj_set_style_text_font(b, F16, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_SEL), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(b, lv_color_hex(C_CY), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, lv_color_hex(C_CY), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_buttonmatrix_set_button_ctrl_all(b, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(b, true);
}

/* ============================================================ apply/state */
#define SCREEN_IDLE_MS (5U * 60U * 1000U)
static bool s_screen_asleep;
static lv_obj_t *s_wake_cover;

void ui_apply_display(void)
{
    LOCK();
    s_screen_asleep = false;
    if (s_wake_cover) {
        lv_obj_delete(s_wake_cover);
        s_wake_cover = NULL;
    }
    lv_display_trigger_activity(NULL);
    int b = g_cfg.power_save ? (g_cfg.brightness * 6 / 10) : g_cfg.brightness;
    if (b < 10) b = 10;
    board_set_brightness((uint8_t)b);
    if (pg[1]) camera_set_active(!lv_obj_has_flag(pg[1], LV_OBJ_FLAG_HIDDEN));
    UNLOCK();
}

static void screen_wake_cb(lv_event_t *e)
{
    (void)e;
    /* Consume the wake touch so it cannot also start TALK or change settings. */
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);
    ui_apply_display();
    ESP_LOGI("ui", "screen awake after touch");
}

static void screen_idle_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!g_cfg.power_save || s_screen_asleep ||
        lv_display_get_inactive_time(NULL) < SCREEN_IDLE_MS) return;

    /* Leave LVGL and the touch controller running to receive the wake touch. */
    s_wake_cover = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake_cover);
    lv_obj_set_pos(s_wake_cover, 0, 0);
    lv_obj_set_size(s_wake_cover, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_wake_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_wake_cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_wake_cover, screen_wake_cb, LV_EVENT_PRESSED, NULL);
    s_screen_asleep = true;
    camera_set_active(false);
    board_set_brightness(0);
    ESP_LOGI("ui", "screen off after five minutes without touch");
}

static void refresh_eco(void)
{
    if (g_cfg.power_save) lv_obj_remove_flag(lb_eco, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(lb_eco, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_model(void)
{
    lv_label_set_text_fmt(lb_model, "%s | %s", settings_provider(), odin_selected_model());
}

static void rebuild_model_map_locked(void)
{
    int count = odin_model_count();
    if (count > 10) count = 10;
    s_model_opts[0] = 0;
    for (int i = 0; i < count; i++) {
        const char *id = odin_model_name(i);
        if (i) strlcat(s_model_opts, "\n", sizeof s_model_opts);
        strlcat(s_model_opts, id, sizeof s_model_opts);
    }
    if (count == 0) {
        strlcpy(s_model_opts, "(loading...)", sizeof s_model_opts);
    }
    if (dd_model) {
        lv_dropdown_set_options(dd_model, s_model_opts);
        if (odin_model_count() > 0) {
            uint32_t sel = g_cfg.model_idx < (uint8_t)odin_model_count() ? g_cfg.model_idx : 0;
            lv_dropdown_set_selected(dd_model, sel);
        }
    }
    refresh_model();
}

void ui_refresh_model_list(void)
{
    if (lvgl_port_lock(0)) {
        rebuild_model_map_locked();
        lvgl_port_unlock();
    }
}

static void refresh_camtag(void)
{
    if (s_camera_unavailable) {
        lv_label_set_text(lb_camtag, "CAMERA UNAVAILABLE");
        return;
    }
    lv_label_set_text_fmt(lb_camtag, "%s  |  %s",
                          s_camera_live ? "LIVE" : "PREVIEW", camera_res_name(g_cfg.cam_res));
}

void ui_camera_status(const char *message)
{
    LOCK();
    s_camera_live = false;
    lv_label_set_text(s_camera_message, message);
    lv_obj_remove_flag(s_camera_message, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
    refresh_camtag();
    UNLOCK();
}

void ui_camera_availability(bool available)
{
    LOCK();
    s_camera_unavailable = !available;
    if (available) {
        lv_obj_remove_state(btn_res, LV_STATE_DISABLED);
        lv_obj_remove_state(sw_aud, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn_res, LV_STATE_DISABLED);
        lv_obj_add_state(sw_aud, LV_STATE_DISABLED);
    }
    ui_camera_status(available ? "Camera ready" : "Camera unavailable");
    UNLOCK();
}

void ui_camera_frame(const uint16_t *pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0) return;
    LOCK();
    if (s_screen_asleep || lv_obj_has_flag(pg[1], LV_OBJ_FLAG_HIDDEN)) { UNLOCK(); return; }
    /* Fit the image into the panel while retaining its aspect ratio. */
    const int max_width = 612, max_height = 470;
    int w = max_width, h = height * max_width / width;
    if (h > max_height) { h = max_height; w = width * max_height / height; }
    if (!s_camera_pixels) {
        s_camera_pixels = heap_caps_malloc(max_width * max_height * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_camera_pixels) { UNLOCK(); ui_camera_status("Preview: out of memory"); return; }
    }
    /* The camera task is the sole producer. Reuse the nearest-neighbour
     * mapping until the capture dimensions change, avoiding a division for
     * every output pixel while LVGL's lock is held. Keep writes protected:
     * LVGL may still be reading the displayed buffer on its own task. */
    static uint16_t source_x[612], source_y[470];
    static int mapped_width, mapped_height;
    if (mapped_width != width || mapped_height != height) {
        for (int x = 0; x < w; x++) source_x[x] = x * width / w;
        for (int y = 0; y < h; y++) source_y[y] = y * height / h;
        mapped_width = width; mapped_height = height;
    }
    for (int y = 0; y < h; y++) {
        const uint16_t *row = pixels + (size_t)source_y[y] * width;
        uint16_t *out = s_camera_pixels + (size_t)y * w;
        for (int x = 0; x < w; x++) out[x] = row[source_x[x]];
    }
    s_camera_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_camera_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_camera_dsc.header.w = w;
    s_camera_dsc.header.h = h;
    s_camera_dsc.header.stride = w * 2;
    s_camera_dsc.data_size = w * h * 2;
    s_camera_dsc.data = (const uint8_t *)s_camera_pixels;
    lv_image_set_src(s_camera_image, &s_camera_dsc);
    lv_obj_align(s_camera_image, LV_ALIGN_CENTER, 0, 12);
    lv_obj_remove_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_camera_message, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_camera_image);
    if (!s_camera_live) { s_camera_live = true; refresh_camtag(); }
    UNLOCK();
}

/* ============================================================== waveform */
static void wave_tick(lv_timer_t *t)
{
    (void)t;
    if (lv_obj_has_flag(pg[0], LV_OBJ_FLAG_HIDDEN)) return;

    float target = s_state == ODIN_RESPONDING ? 1.0f : s_state == ODIN_LISTENING ? 0.55f : 0.10f;
    s_amp += (target - s_amp) * 0.06f;
    s_t += 0.05f;

    for (int l = 0; l < 3; l++) {
        for (int i = 0; i <= WAVE_N; i++) {
            float n = (float)i / WAVE_N;
            float env = sinf(n * 3.14159265f);
            float y = WAVE_H / 2.0f
                    + sinf(n * 14.0f + s_t * (2 + l)) * WAVE_H * 0.38f * s_amp * env
                      * (sinf(s_t * 1.7f + n * 5.0f + l) * 0.4f + 0.6f)
                    + sinf(n * 30.0f - s_t * 3.0f) * WAVE_H * 0.06f * s_amp * env;
            wave_pts[l][i].x = (lv_value_precise_t)(n * WAVE_W);
            wave_pts[l][i].y = (lv_value_precise_t)y;
        }
        lv_line_set_points(wave[l], wave_pts[l], WAVE_N + 1);
    }
}

/* ============================================================ typed reply */
static void scroll_resp_bottom(void)
{
    lv_obj_update_layout(resp_cont);
    int32_t b = lv_obj_get_scroll_bottom(resp_cont);
    if (b > 0) lv_obj_scroll_by(resp_cont, 0, -b, LV_ANIM_OFF);
}

static void type_tick(lv_timer_t *t)
{
    for (int k = 0; k < 2 && s_resp_pos < s_resp_len; k++) {
        s_resp_pos++;
        while (s_resp_pos < s_resp_len && ((unsigned char)s_resp[s_resp_pos] & 0xC0) == 0x80) s_resp_pos++;
    }
    memcpy(s_shown, s_resp, s_resp_pos);
    s_shown[s_resp_pos] = 0;
    lv_label_set_text(lb_resp, s_shown);
    scroll_resp_bottom();
    if (s_resp_pos >= s_resp_len) {
        lv_timer_delete(t);
        s_type_timer = NULL;
    }
}

void ui_show_response(const char *text)
{
    LOCK();
    if (s_type_timer) { lv_timer_delete(s_type_timer); s_type_timer = NULL; }
    strlcpy(s_resp, text ? text : "", sizeof s_resp);
    s_resp_len = strlen(s_resp);
    s_resp_pos = 0;
    lv_label_set_text(lb_resp, "");
    s_type_timer = lv_timer_create(type_tick, 35, NULL);
    UNLOCK();
}

void ui_show_query(const char *text)
{
    LOCK();
    lv_label_set_text(lb_query, text ? text : "");
    UNLOCK();
}

/* ============================================================== callbacks */
static void show_page(int p)
{
    camera_set_active(p == 1 && !s_screen_asleep);
    if (p != 0) odin_talk_released();
    for (int i = 0; i < 3; i++) {
        if (i == p) lv_obj_remove_flag(pg[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pg[i], LV_OBJ_FLAG_HIDDEN);
        if (i == p) lv_obj_add_state(nav[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(nav[i], LV_STATE_CHECKED);
    }
    if (p == 0) lv_obj_remove_flag(talk_btn, LV_OBJ_FLAG_HIDDEN);   /* TALK only on Home */
    else lv_obj_add_flag(talk_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void nav_cb(lv_event_t *e) { show_page((int)(intptr_t)lv_event_get_user_data(e)); }
static void talk_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) odin_talk_pressed();
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) odin_talk_released();
}

static void vol_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) { settings_save(); return; }
    settings_lock();
    g_cfg.volume = (uint8_t)lv_slider_get_value(sl_vol);
    lv_label_set_text_fmt(lb_vol, "%d%%", (int)g_cfg.volume);
    board_set_volume(g_cfg.volume);
    settings_unlock();
}

static void bri_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) { settings_save(); return; }
    settings_lock();
    g_cfg.brightness = (uint8_t)lv_slider_get_value(sl_bri);
    lv_label_set_text_fmt(lb_bri, "%d%%", (int)g_cfg.brightness);
    ui_apply_display();
    settings_unlock();
}

static void pow_cb(lv_event_t *e)
{
    settings_lock();
    (void)e;
    g_cfg.power_save = lv_obj_has_state(sw_pow, LV_STATE_CHECKED);
    refresh_eco();
    ui_apply_display();
    settings_save();
    settings_unlock();
}

static void model_cb(lv_event_t *e)
{
    (void)e;
    uint32_t id = lv_dropdown_get_selected(dd_model);
    if ((int)id < odin_model_count()) {
        odin_select_model((int)id);
        refresh_model();
    }
}

static void ta_cb(lv_event_t *e)
{
    settings_lock();
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(kb, ta_tts);
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
    settings_unlock();
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) {
        if (odin_state() != ODIN_STANDBY) { lv_label_set_text(lb_tts, "ASSISTANT BUSY"); return; }
        settings_lock();
        strlcpy(g_cfg.tts_url, lv_textarea_get_text(ta_tts), sizeof g_cfg.tts_url);
        settings_save();
        settings_unlock();
        odin_tts_settings_changed();
        lv_label_set_text(lb_tts, "TTS CHECKING");
        odin_tts_test();
    } else if (c == LV_EVENT_CANCEL) {
        lv_textarea_set_text(ta_tts, g_cfg.tts_url);
    } else {
        return;
    }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(kb, NULL);
}

static void tts_test_cb(lv_event_t *e)
{
    (void)e;
    ui_set_tts(0);
    odin_tts_test();
}

static void res_cb(lv_event_t *e)
{
    settings_lock();
    (void)e;
    uint32_t id = lv_buttonmatrix_get_selected_button(btn_res);
    if (id < BOARD_CAMERA_RES_COUNT) {
        g_cfg.cam_res = (uint8_t)id;
        refresh_camtag();
        camera_apply();
        settings_save();
    }
    settings_unlock();
}

static void aud_cb(lv_event_t *e)
{
    settings_lock();
    (void)e;
    g_cfg.cam_audio = lv_obj_has_state(sw_aud, LV_STATE_CHECKED);
    refresh_camtag();
    camera_apply();
    settings_save();
    settings_unlock();
}

/* =============================================================== builders */
static lv_obj_t *mk_navbtn(lv_obj_t *parent, const char *sym, const char *txt, int grow)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_flex_grow(b, grow);
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_add_style(b, &st_btn_on, LV_STATE_CHECKED);
    lv_obj_add_style(b, &st_btn_on, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 6, 0);
    mk_label(b, sym, F30, C_CY);
    lv_obj_t *t = mk_label(b, txt, F12, C_DIM);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    return b;
}

static void build_topbar(lv_obj_t *scr)
{
    lv_obj_t *bar = mk_panel(scr, 6, 6, 1012, 44);
    lv_obj_set_style_pad_hor(bar, 14, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);

    lv_obj_t *brand = mk_label(bar, "ODIN", F24, C_CY);
    lv_obj_set_style_text_letter_space(brand, 6, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *r = mk_clear(bar);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 14, 0);

    lb_wifi = mk_label(r, LV_SYMBOL_WIFI "  offline", F14, C_ERR);

    lv_obj_t *g = mk_clear(r);
    lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g, 6, 0);
    led_llm = lv_led_create(g);
    lv_obj_set_size(led_llm, 10, 10);
    lv_led_set_color(led_llm, lv_color_hex(C_ERR));
    lv_led_on(led_llm);
    lb_llm = mk_label(g, "LLM OFFLINE", F14, C_TX);

    lb_model = mk_label(r, "", F14, C_MG);
    lv_obj_set_width(lb_model, 220);
    lv_label_set_long_mode(lb_model, LV_LABEL_LONG_DOT);
    refresh_model();
    lb_eco = mk_label(r, "ECO", F14, C_OK);
    lb_bat = mk_label(r, LV_SYMBOL_BATTERY_EMPTY " --%", F14, C_DIM);
}

static void build_home(lv_obj_t *scr)
{
    pg[0] = mk_clear(scr);
    lv_obj_set_pos(pg[0], 6, 56);
    lv_obj_set_size(pg[0], 896, 538);

    /* time */
    lv_obj_t *p = mk_panel(pg[0], 0, 0, 230, 150);
    mk_cap(p, "LOCAL TIME");
    lv_obj_t *row = mk_clear(p);
    lv_obj_set_pos(row, 0, 20);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lb_time = mk_label(row, "--:--", F48, C_CY);
    lb_sec = mk_label(row, "--", F24, C_MG);
    lb_date = mk_label(p, "Syncing time...", F14, C_TX);
    lv_obj_set_width(lb_date, LV_PCT(100));
    lv_label_set_long_mode(lb_date, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lb_date, 0, 86);

    /* weather */
    p = mk_panel(pg[0], 0, 156, 230, 296);
    s_weather_location = mk_cap(p, g_cfg.weather_city);
    lv_obj_set_width(s_weather_location, 200);
    lv_label_set_long_mode(s_weather_location, LV_LABEL_LONG_DOT);
    lb_temp = mk_label(p, "--" "\xC2\xB0" "C", F42, C_TX);
    lv_obj_set_pos(lb_temp, 0, 24);
    lb_wdesc = mk_label(p, "Weather unavailable", F16, C_CY);
    lv_obj_set_width(lb_wdesc, LV_PCT(100));
    lv_label_set_long_mode(lb_wdesc, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lb_wdesc, 0, 82);
    lv_obj_t *kv = mk_clear(p);
    lv_obj_set_size(kv, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(kv, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(kv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(kv, 8, 0);
    mk_kv(kv, "Feels like", &lb_feels);
    mk_kv(kv, "Wind", &lb_wind);
    mk_kv(kv, "Humidity", &lb_hum);

    /* ip */
    p = mk_panel(pg[0], 0, 458, 230, 80);
    mk_cap(p, "IP ADDRESS");
    lb_ip = mk_label(p, "--", F20, C_CY);
    lv_obj_set_pos(lb_ip, 0, 26);

    /* orb / waveform */
    p = mk_panel(pg[0], 236, 0, 660, 250);
    lb_state = mk_label(p, "STANDBY", F12, C_CY);
    lv_obj_set_style_text_letter_space(lb_state, 4, 0);
    const uint32_t cols[3] = { C_CY, C_MG, C_OK };
    for (int l = 0; l < 3; l++) {
        wave[l] = lv_line_create(p);
        lv_obj_set_pos(wave[l], 0, 30);
        lv_obj_set_style_line_width(wave[l], 3, 0);
        lv_obj_set_style_line_rounded(wave[l], true, 0);
        lv_obj_set_style_line_color(wave[l], lv_color_hex(cols[l]), 0);
        lv_obj_set_style_line_opa(wave[l], (lv_opa_t)(255 - l * 60), 0);
    }
    lv_obj_t *name = mk_label(p, "ODIN", F30, C_TX);
    lv_obj_set_style_text_letter_space(name, 14, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 7, 0);

    /* response */
    p = mk_panel(pg[0], 236, 256, 660, 282);
    mk_cap(p, "RESPONSE");
    lb_query = mk_label(p, "Hold TALK to speak; release to send", F14, C_DIM);
    lv_obj_set_pos(lb_query, 0, 20);
    resp_cont = mk_clear(p);
    lv_obj_add_flag(resp_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(resp_cont, LV_DIR_VER);
    lv_obj_set_pos(resp_cont, 0, 48);
    lv_obj_set_size(resp_cont, 636, 210);
    lb_resp = mk_label(resp_cont, "Awaiting your command.", F20, C_TX);
    lv_obj_set_width(lb_resp, LV_PCT(100));
    lv_label_set_long_mode(lb_resp, LV_LABEL_LONG_WRAP);

    lv_timer_create(wave_tick, 33, NULL);
}

static lv_obj_t *mk_row_head(lv_obj_t *p, const char *title, lv_obj_t **val)
{
    lv_obj_t *row = mk_clear(p);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mk_label(row, title, F16, C_TX);
    if (val) *val = mk_label(row, "", F16, C_CY);
    return row;
}

static void build_settings(lv_obj_t *scr)
{
    pg[2] = mk_clear(scr);
    lv_obj_set_pos(pg[2], 6, 56);
    lv_obj_set_size(pg[2], 896, 538);

    /* --- audio & display --- */
    lv_obj_t *p = mk_panel(pg[2], 0, 0, 445, 538);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    mk_cap(p, "AUDIO & DISPLAY");

    mk_row_head(p, "System volume", &lb_vol);
    lv_label_set_text_fmt(lb_vol, "%d%%", (int)g_cfg.volume);
    sl_vol = lv_slider_create(p);
    style_slider(sl_vol);
    lv_slider_set_range(sl_vol, 0, 100);
    lv_slider_set_value(sl_vol, g_cfg.volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl_vol, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl_vol, vol_cb, LV_EVENT_RELEASED, NULL);

    mk_row_head(p, "Screen brightness", &lb_bri);
    lv_label_set_text_fmt(lb_bri, "%d%%", (int)g_cfg.brightness);
    sl_bri = lv_slider_create(p);
    style_slider(sl_bri);
    lv_slider_set_range(sl_bri, 20, 100);
    lv_slider_set_value(sl_bri, g_cfg.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl_bri, bri_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl_bri, bri_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *row = mk_row_head(p, "Power saving mode", NULL);
    sw_pow = lv_switch_create(row);
    style_switch(sw_pow);
    if (g_cfg.power_save) lv_obj_add_state(sw_pow, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_pow, pow_cb, LV_EVENT_VALUE_CHANGED, NULL);
    mk_label(p, "Screen off after 5 min idle. Touch to wake.", F14, C_DIM);
    s_web_address = mk_label(p, "Web settings: starting...", F14, C_CY);
    s_web_credentials = mk_label(p, "", F12, C_DIM);
    if (settings_web_password_custom()) lv_obj_add_flag(s_web_credentials, LV_OBJ_FLAG_HIDDEN);
    else lv_label_set_text_fmt(s_web_credentials, "User: admin | Password: %s", g_cfg.web_password);

    /* --- assistant --- */
    p = mk_panel(pg[2], 451, 0, 445, 538);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 16, 0);
    mk_cap(p, "ASSISTANT");

    mk_label(p, "AI model", F16, C_TX);
    dd_model = lv_dropdown_create(p);
    lv_obj_set_width(dd_model, LV_PCT(100));
    lv_dropdown_set_symbol(dd_model, LV_SYMBOL_DOWN);
    lv_obj_set_style_bg_color(dd_model, lv_color_hex(0x031019), 0);
    lv_obj_set_style_border_color(dd_model, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_color(dd_model, lv_color_hex(C_CY), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(dd_model, lv_color_hex(C_CY), 0);
    lv_obj_set_style_text_font(dd_model, F14, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd_model);
    if (list) {
        lv_obj_set_style_bg_color(list, lv_color_hex(0x031019), 0);
        lv_obj_set_style_border_color(list, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_text_color(list, lv_color_hex(C_TX), 0);
        lv_obj_set_style_text_font(list, F14, 0);
    }
    rebuild_model_map_locked();
    lv_obj_add_event_cb(dd_model, model_cb, LV_EVENT_VALUE_CHANGED, NULL);

    mk_label(p, "TTS server address", F16, C_TX);
    ta_tts = lv_textarea_create(p);
    lv_textarea_set_one_line(ta_tts, true);
    lv_textarea_set_max_length(ta_tts, sizeof(g_cfg.tts_url) - 1);
    lv_textarea_set_text(ta_tts, g_cfg.tts_url);
    lv_obj_set_width(ta_tts, LV_PCT(100));
    lv_obj_set_style_bg_color(ta_tts, lv_color_hex(0x031019), 0);
    lv_obj_set_style_border_color(ta_tts, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_color(ta_tts, lv_color_hex(C_CY), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(ta_tts, lv_color_hex(C_CY), 0);
    lv_obj_set_style_text_font(ta_tts, F16, 0);
    lv_obj_add_event_cb(ta_tts, ta_cb, LV_EVENT_FOCUSED, NULL);

    row = mk_clear(p);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *st = mk_clear(row);
    lv_obj_set_size(st, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, 8, 0);
    led_tts = lv_led_create(st);
    lv_obj_set_size(led_tts, 12, 12);
    lv_led_set_color(led_tts, lv_color_hex(C_WARN));
    lv_led_on(led_tts);
    lb_tts = mk_label(st, "TTS CHECKING", F14, C_TX);
    lv_obj_t *tb = lv_button_create(row);
    lv_obj_add_style(tb, &st_btn, 0);
    lv_obj_add_style(tb, &st_btn_on, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(tb, 20, 0);
    lv_obj_set_style_pad_ver(tb, 10, 0);
    mk_label(tb, "TEST", F14, C_CY);
    lv_obj_add_event_cb(tb, tts_test_cb, LV_EVENT_CLICKED, NULL);

    /* on-screen keyboard (shared) */
    kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(kb, 1024, 260);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x04121c), 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x0a1c2b), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(C_TX), LV_PART_ITEMS);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_ALL, NULL);
}

static void build_camera(lv_obj_t *scr)
{
    pg[1] = mk_clear(scr);
    lv_obj_set_pos(pg[1], 6, 56);
    lv_obj_set_size(pg[1], 896, 538);

    lv_obj_t *v = mk_panel(pg[1], 0, 0, 640, 538);
    lv_obj_set_style_bg_color(v, lv_color_hex(0x02080e), 0);
    lb_camtag = mk_label(v, "", F12, C_WARN);
    lv_obj_set_style_text_letter_space(lb_camtag, 3, 0);
    s_camera_image = lv_image_create(v);
    lv_obj_add_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
    s_camera_message = mk_label(v, "Checking camera...", F20, C_DIM);
    lv_obj_set_width(s_camera_message, 580);
    lv_obj_set_style_text_align(s_camera_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_camera_message);

    lv_obj_t *p = mk_panel(pg[1], 646, 0, 250, 538);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 16, 0);
    mk_cap(p, "CAMERA PREVIEW");
    mk_label(p, "Resolution", F16, C_TX);
    static const char *res_map[] = { "1024x600", "\n", "640x480", "\n", "800x800", "" };
    btn_res = lv_buttonmatrix_create(p);
    lv_buttonmatrix_set_map(btn_res, res_map);
    lv_obj_set_size(btn_res, LV_PCT(100), 240);
    style_btnm(btn_res);
    lv_buttonmatrix_set_selected_button(btn_res, g_cfg.cam_res < BOARD_CAMERA_RES_COUNT ?
                                       g_cfg.cam_res : LV_BUTTONMATRIX_BUTTON_NONE);
    if (g_cfg.cam_res < BOARD_CAMERA_RES_COUNT)
        lv_buttonmatrix_set_button_ctrl(btn_res, g_cfg.cam_res, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_add_event_cb(btn_res, res_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_state(btn_res, LV_STATE_DISABLED);

    lv_obj_t *sep = mk_cap(p, "AUDIO");
    lv_obj_set_style_pad_top(sep, 20, 0);
    lv_obj_t *row = mk_row_head(p, "Stream audio", NULL);
    sw_aud = lv_switch_create(row);
    style_switch(sw_aud);
    if (g_cfg.cam_audio) lv_obj_add_state(sw_aud, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_aud, aud_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_state(sw_aud, LV_STATE_DISABLED);
    refresh_camtag();
}

static void build_nav(lv_obj_t *scr)
{
    lv_obj_t *col = mk_clear(scr);
    lv_obj_set_pos(col, 908, 56);
    lv_obj_set_size(col, 110, 538);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);

    nav[0] = mk_navbtn(col, LV_SYMBOL_HOME, "HOME", 2);
    nav[1] = mk_navbtn(col, LV_SYMBOL_VIDEO, "CAMERA", 2);

    talk_btn = mk_navbtn(col, LV_SYMBOL_AUDIO, "TALK", 3);
    lv_obj_set_style_border_color(talk_btn, lv_color_hex(C_MG), 0);
    lv_obj_add_style(talk_btn, &st_talk_on, LV_STATE_CHECKED);
    talk_lbl = lv_obj_get_child(talk_btn, 1);
    lv_obj_add_event_cb(talk_btn, talk_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(talk_btn, talk_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(talk_btn, talk_cb, LV_EVENT_PRESS_LOST, NULL);

    nav[2] = mk_navbtn(col, LV_SYMBOL_SETTINGS, "SETTINGS", 2);
    for (int i = 0; i < 3; i++) lv_obj_add_event_cb(nav[i], nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
}

void ui_init(void)
{
    LOCK();
    lv_style_init(&st_btn);
    lv_style_set_bg_color(&st_btn, lv_color_hex(C_PANEL));
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_border_color(&st_btn, lv_color_hex(C_LINE));
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_radius(&st_btn, 10);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_pad_all(&st_btn, 4);

    lv_style_init(&st_btn_on);
    lv_style_set_bg_color(&st_btn_on, lv_color_hex(C_SEL));
    lv_style_set_border_color(&st_btn_on, lv_color_hex(C_CY));
    lv_style_set_border_width(&st_btn_on, 2);

    lv_style_init(&st_talk_on);
    lv_style_set_bg_color(&st_talk_on, lv_color_hex(0x3a1260));
    lv_style_set_border_color(&st_talk_on, lv_color_hex(C_MG));
    lv_style_set_border_width(&st_talk_on, 2);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_topbar(scr);
    build_home(scr);
    build_camera(scr);
    build_settings(scr);
    build_nav(scr);
    refresh_eco();
    show_page(0);
    lv_display_trigger_activity(NULL);
    lv_timer_create(screen_idle_tick, 500, NULL);
    UNLOCK();
}

/* =============================================================== setters */
void ui_set_clock(const struct tm *t)
{
    LOCK();
    if (t) {
        char d[48];
        lv_label_set_text_fmt(lb_time, "%02d:%02d", t->tm_hour, t->tm_min);
        lv_label_set_text_fmt(lb_sec, "%02d", t->tm_sec);
        strftime(d, sizeof d, "%A, %B %d, %Y", t);
        lv_label_set_text(lb_date, d);
    } else {
        lv_label_set_text(lb_time, "--:--");
        lv_label_set_text(lb_sec, "--");
        lv_label_set_text(lb_date, "Syncing time...");
    }
    UNLOCK();
}

void ui_set_wifi(bool connected, int rssi)
{
    LOCK();
    if (connected) {
        if (rssi) lv_label_set_text_fmt(lb_wifi, LV_SYMBOL_WIFI "  %d dBm", rssi);
        else lv_label_set_text(lb_wifi, LV_SYMBOL_WIFI "  connected");
        set_col(lb_wifi, C_OK);
    } else {
        lv_label_set_text(lb_wifi, LV_SYMBOL_WIFI "  offline");
        set_col(lb_wifi, C_ERR);
    }
    UNLOCK();
}

void ui_set_ip(const char *ip)
{
    LOCK();
    lv_label_set_text(lb_ip, ip);
    if (net_connected()) lv_label_set_text_fmt(s_web_address, "Web: http://%s/", ip);
    else lv_label_set_text_fmt(s_web_address, "%s: http://192.168.4.1/", net_setup_ssid());
    UNLOCK();
}

void ui_set_weather(bool valid, float temp, float feels, int humidity, float wind_kmh, const char *desc)
{
    LOCK();
    if (valid) {
        lv_label_set_text_fmt(lb_temp, "%d\xC2\xB0" "C", (int)lroundf(temp));
        lv_label_set_text(lb_wdesc, desc);
        lv_label_set_text_fmt(lb_feels, "%d\xC2\xB0" "C", (int)lroundf(feels));
        lv_label_set_text_fmt(lb_wind, "%d km/h", (int)lroundf(wind_kmh));
        lv_label_set_text_fmt(lb_hum, "%d%%", humidity);
    } else {
        lv_label_set_text(lb_temp, "--" "\xC2\xB0" "C");
        lv_label_set_text(lb_wdesc, "Weather unavailable");
    }
    UNLOCK();
}

void ui_set_llm(bool online)
{
    LOCK();
    lv_led_set_color(led_llm, lv_color_hex(online ? C_OK : C_ERR));
    lv_label_set_text(lb_llm, online ? "LLM ONLINE" : "LLM OFFLINE");
    UNLOCK();
}

void ui_set_battery(int percent, int millivolts)
{
    LOCK();
    if (percent == -2) {
        lv_label_set_text_fmt(lb_bat, LV_SYMBOL_USB " USB %d.%02dV", millivolts / 1000, (millivolts % 1000) / 10);
        set_col(lb_bat, C_CY);
    } else if (percent < 0) {
        lv_label_set_text(lb_bat, LV_SYMBOL_BATTERY_EMPTY " --%");
        set_col(lb_bat, C_DIM);
    } else {
        const char *sym = percent > 85 ? LV_SYMBOL_BATTERY_FULL : percent > 60 ? LV_SYMBOL_BATTERY_3 :
                          percent > 35 ? LV_SYMBOL_BATTERY_2 : percent > 10 ? LV_SYMBOL_BATTERY_1 :
                          LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(lb_bat, "%s %d%% %d.%02dV", sym, percent, millivolts / 1000, (millivolts % 1000) / 10);
        set_col(lb_bat, percent > 20 ? C_TX : C_ERR);
    }
    UNLOCK();
}

void ui_set_tts(int state)
{
    LOCK();
    static const uint32_t col[3] = { C_WARN, C_OK, C_ERR };
    static const char *txt[3] = { "TTS CHECKING", "TTS WORKING", "TTS FAILED" };
    if (state < 0 || state > 2) state = 2;
    lv_led_set_color(led_tts, lv_color_hex(col[state]));
    lv_label_set_text(lb_tts, txt[state]);
    UNLOCK();
}

void ui_set_state(odin_state_t st)
{
    LOCK();
    s_state = st;
    lv_label_set_text(lb_state, st == ODIN_LISTENING ? "LISTENING" : st == ODIN_PROCESSING ? "PROCESSING" : st == ODIN_RESPONDING ? "RESPONDING" : "STANDBY");
    if (st == ODIN_LISTENING) lv_obj_add_state(talk_btn, LV_STATE_CHECKED);
    else lv_obj_remove_state(talk_btn, LV_STATE_CHECKED);
    lv_label_set_text(talk_lbl, st == ODIN_LISTENING ? "RELEASE" : "TALK");
    UNLOCK();
}

void ui_set_weather_location(const char *city, const char *country)
{
    LOCK();
    lv_label_set_text_fmt(s_weather_location, "%s, %s", city, country);
    UNLOCK();
}
void ui_settings_refresh(void)
{
    LOCK(); settings_lock();
    if (settings_web_password_custom()) {
        lv_label_set_text(s_web_credentials, "");
        lv_obj_add_flag(s_web_credentials, LV_OBJ_FLAG_HIDDEN);
    }
    lv_slider_set_value(sl_vol, g_cfg.volume, LV_ANIM_OFF);
    lv_label_set_text_fmt(lb_vol, "%d%%", g_cfg.volume);
    lv_slider_set_value(sl_bri, g_cfg.brightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(lb_bri, "%d%%", g_cfg.brightness);
    if (g_cfg.power_save) lv_obj_add_state(sw_pow, LV_STATE_CHECKED);
    else lv_obj_remove_state(sw_pow, LV_STATE_CHECKED);
    board_set_volume(g_cfg.volume);
    if (!s_screen_asleep || !g_cfg.power_save) ui_apply_display();
    refresh_eco();
    ui_set_weather_location(g_cfg.weather_city, g_cfg.weather_country_code);
    lv_textarea_set_text(ta_tts, g_cfg.tts_url);
    lv_buttonmatrix_clear_button_ctrl_all(btn_res, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_buttonmatrix_set_selected_button(btn_res, g_cfg.cam_res < BOARD_CAMERA_RES_COUNT ?
                                       g_cfg.cam_res : LV_BUTTONMATRIX_BUTTON_NONE);
    if (g_cfg.cam_res < BOARD_CAMERA_RES_COUNT)
        lv_buttonmatrix_set_button_ctrl(btn_res, g_cfg.cam_res, LV_BUTTONMATRIX_CTRL_CHECKED);
    if (g_cfg.cam_audio) lv_obj_add_state(sw_aud, LV_STATE_CHECKED);
    else lv_obj_remove_state(sw_aud, LV_STATE_CHECKED);
    refresh_camtag(); refresh_model();
    settings_unlock(); UNLOCK();
}
