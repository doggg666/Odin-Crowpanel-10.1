#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { ODIN_STANDBY = 0, ODIN_LISTENING, ODIN_PROCESSING, ODIN_RESPONDING } odin_state_t;

void odin_init(void);
void odin_talk_pressed(void);   /* Press starts recording; press during speech cancels it. */
void odin_talk_released(void);  /* Release or lost touch ends recording. */
bool odin_tts_test(void);       /* re-check the TTS server now */
bool odin_llm_online(void);     /* provider reachable */
int  odin_tts_state(void);      /* 0 checking, 1 connected, 2 unreachable */

/* LLM model list (fetched from the configured LM Studio / OpenAI endpoint) */
int         odin_model_count(void);
const char *odin_model_name(int idx);
const char *odin_selected_model(void);
void        odin_select_model(int idx); /* persists + updates selection */
void        odin_fetch_models(void);    /* refresh list now (async-safe to call) */

void odin_settings_changed(void);
void odin_tts_detail(char *out, size_t size);
int odin_state(void);
bool odin_chat_test(const char *text, char *out, size_t size);

void odin_tts_settings_changed(void);
