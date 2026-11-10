#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CAMERA_RES_COUNT 5
#define CAMERA_RES_DEFAULT 0

const char *camera_res_name(uint8_t idx);
void camera_init(void);
bool camera_available(void); /* result of boot-time detection */
void camera_set_active(bool active);
/* Apply the selected capture resolution asynchronously. */
void camera_apply(void);

#define CAMERA_JPEG_CAPACITY (1536U * 1024U)
bool camera_stream_open(void);
void camera_stream_close(void);
unsigned camera_stream_clients(void);
bool camera_copy_jpeg(uint8_t *out, size_t capacity, size_t *length, uint32_t *sequence);
int camera_res_width(unsigned index);
int camera_res_height(unsigned index);
