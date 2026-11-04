#include "odin_ui.h"

static lv_obj_t *status_label;
static lv_obj_t *transcript_label;
static lv_obj_t *mode_label;

void odin_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xEAF4FF), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ODIN");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 32, 24);

    mode_label = lv_label_create(scr);
    lv_label_set_text(mode_label, "AI: LOCAL");
    lv_obj_set_style_text_color(mode_label, lv_color_hex(0x55D6FF), 0);
    lv_obj_align(mode_label, LV_ALIGN_TOP_RIGHT, -32, 34);

    lv_obj_t *orb = lv_obj_create(scr);
    lv_obj_set_size(orb, 190, 190);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb, lv_color_hex(0x123B60), 0);
    lv_obj_set_style_border_color(orb, lv_color_hex(0x36CFFF), 0);
    lv_obj_set_style_border_width(orb, 4, 0);
    lv_obj_align(orb, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *orb_text = lv_label_create(orb);
    lv_label_set_text(orb_text, "ODIN");
    lv_obj_center(orb_text);

    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Starting...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x55D6FF), 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 100);

    transcript_label = lv_label_create(scr);
    lv_label_set_text(transcript_label, "Welcome. Odin is initializing.");
    lv_label_set_long_mode(transcript_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(transcript_label, 850);
    lv_obj_set_style_text_align(transcript_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(transcript_label, LV_ALIGN_BOTTOM_MID, 0, -55);
}

void odin_ui_set_status(const char *status)
{
    if (status_label && status) lv_label_set_text(status_label, status);
}

void odin_ui_set_transcript(const char *text)
{
    if (transcript_label && text) lv_label_set_text(transcript_label, text);
}

void odin_ui_set_mode(const char *mode)
{
    if (mode_label && mode) {
        char buf[48];
        lv_snprintf(buf, sizeof(buf), "AI: %s", mode);
        lv_label_set_text(mode_label, buf);
    }
}
