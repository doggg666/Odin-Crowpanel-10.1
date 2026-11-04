#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void odin_ui_create(void);
void odin_ui_set_status(const char *status);
void odin_ui_set_transcript(const char *text);
void odin_ui_set_mode(const char *mode);

#ifdef __cplusplus
}
#endif
