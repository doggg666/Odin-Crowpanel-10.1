#include "camera.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_device_internal.h"
#include "esp_video_ioctl.h"
#include "esp_cam_sensor.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "camera";
static const char *k_res[CAMERA_RES_COUNT] = { "1024x600", "640x480", "800x800", "1280x720", "1920x1080" };
static const uint16_t k_width[CAMERA_RES_COUNT] = {1024, 640, 800, 1280, 1920};
static const uint16_t k_height[CAMERA_RES_COUNT] = {600, 480, 800, 720, 1080};
static TaskHandle_t s_task;
static atomic_bool s_active;
static atomic_int s_resolution;
static atomic_bool s_available;
static atomic_uint s_stream_clients;
static atomic_bool s_fault;
static atomic_int s_fps = 5, s_quality = 70;
static SemaphoreHandle_t s_jpeg_mutex;
static jpeg_encoder_handle_t s_encoder;
static uint8_t *s_jpeg;
static size_t s_jpeg_capacity, s_jpeg_length;
static uint32_t s_jpeg_sequence;
static bool capture_requested(void)
{
    return !atomic_load(&s_fault) && (atomic_load(&s_active) || atomic_load(&s_stream_clients));
}
int camera_res_width(unsigned index) { return k_width[index < CAMERA_RES_COUNT ? index : 0]; }
int camera_res_height(unsigned index) { return k_height[index < CAMERA_RES_COUNT ? index : 0]; }
unsigned camera_stream_clients(void) { return atomic_load(&s_stream_clients); }

static bool encode_frame(const void *pixels, int width, int height)
{
    if (!s_encoder) {
        jpeg_encode_engine_cfg_t cfg = {.timeout_ms = 500};
        if (jpeg_new_encoder_engine(&cfg, &s_encoder) != ESP_OK) return false;
    }
    if (!s_jpeg) {
        jpeg_encode_memory_alloc_cfg_t memory = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
        size_t capacity = 0;
        uint8_t *memory_buffer = jpeg_alloc_encoder_mem(CAMERA_JPEG_CAPACITY, &memory, &capacity);
        if (!memory_buffer) return false;
        xSemaphoreTake(s_jpeg_mutex, portMAX_DELAY);
        s_jpeg = memory_buffer; s_jpeg_capacity = capacity;
        xSemaphoreGive(s_jpeg_mutex);
    }
    jpeg_encode_cfg_t cfg = {.width = width, .height = height, .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420, .image_quality = atomic_load(&s_quality)};
    xSemaphoreTake(s_jpeg_mutex, portMAX_DELAY);
    uint32_t size = 0;
    esp_err_t err = jpeg_encoder_process(s_encoder, &cfg, pixels, width * height * 2,
                                        s_jpeg, s_jpeg_capacity, &size);
    if (err == ESP_OK) { s_jpeg_length = size; s_jpeg_sequence++; }
    else { s_jpeg_length = 0; ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(err)); }
    xSemaphoreGive(s_jpeg_mutex);
    return err == ESP_OK;
}

bool camera_copy_jpeg(uint8_t *out, size_t capacity, size_t *length, uint32_t *sequence)
{
    if (!s_jpeg_mutex || xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = s_jpeg && s_jpeg_length && s_jpeg_length <= capacity && *sequence != s_jpeg_sequence;
    if (ok) { memcpy(out, s_jpeg, s_jpeg_length); *length = s_jpeg_length; *sequence = s_jpeg_sequence; }
    xSemaphoreGive(s_jpeg_mutex);
    return ok;
}
bool camera_stream_open(void)
{
    if (!camera_available()) return false;
    unsigned clients = atomic_load(&s_stream_clients);
    while (clients < 2) {
        if (atomic_compare_exchange_weak(&s_stream_clients, &clients, clients + 1)) {
            atomic_store(&s_fault, false);
            if (!clients) { xSemaphoreTake(s_jpeg_mutex, portMAX_DELAY); s_jpeg_length = 0; xSemaphoreGive(s_jpeg_mutex); }
            if (s_task) xTaskNotifyGive(s_task);
            return true;
        }
    }
    return false;
}
void camera_stream_close(void)
{
    unsigned clients = atomic_load(&s_stream_clients);
    while (clients && !atomic_compare_exchange_weak(&s_stream_clients, &clients, clients - 1)) {}
    if (s_task) xTaskNotifyGive(s_task);
}


#define CAMERA_SCCB_ADDRESS 0x30

static esp_err_t probe_camera(void)
{
    /* Probe only the camera bus; do not disturb the touch/battery I2C bus. */
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t config = {
        .i2c_port = 1, .scl_io_num = 13, .sda_io_num = 12,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&config, &bus);
    if (err != ESP_OK) return err;
    err = i2c_master_probe(bus, CAMERA_SCCB_ADDRESS, 100);
    i2c_del_master_bus(bus);
    return err;
}

bool camera_available(void) { return atomic_load(&s_available); }

const char *camera_res_name(uint8_t idx)
{
    return k_res[idx < CAMERA_RES_COUNT ? idx : CAMERA_RES_DEFAULT];
}

static esp_err_t init_sensor(void)
{
    const esp_video_init_csi_config_t csi = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {.port = 1, .scl_pin = 13, .sda_pin = 12},
            .freq = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
        /* board_init already supplies MIPI PHY power through LDO3. */
        .dont_init_ldo = true,
    };
    const esp_video_init_config_t config = {.csi = &csi};
    return esp_video_init(&config);
}

/* Use the sensor's own immutable format table, including its register scripts. */
static const esp_cam_sensor_format_t *find_format(int resolution)
{
    esp_cam_sensor_device_t *sensor = esp_video_get_csi_video_device_sensor();
    if (!sensor) return NULL;
    esp_cam_sensor_format_array_t formats = {0};
    if (esp_cam_sensor_query_format(sensor, &formats) != ESP_OK) return NULL;
    for (uint32_t i = 0; i < formats.count; i++) {
        const esp_cam_sensor_format_t *f = &formats.format_array[i];
        if (f->width == k_width[resolution] && f->height == k_height[resolution] &&
            f->port == ESP_CAM_SENSOR_MIPI_CSI && f->mipi_info.lane_num == 2) return f;
    }
    return NULL;
}

static void preview_run(int resolution)
{
    int fd = -1;
    bool streaming = false;
    void *buffers[3] = {0};
    size_t lengths[3] = {0};
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ui_camera_status("Starting camera...");
    fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (fd < 0) goto fail;
    const esp_cam_sensor_format_t *sensor_format = find_format(resolution);
    if (!sensor_format || ioctl(fd, VIDIOC_S_SENSOR_FMT, sensor_format) != 0) goto fail;
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {.width = k_width[resolution], .height = k_height[resolution],
                    .pixelformat = V4L2_PIX_FMT_RGB565},
    };
    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0 || ioctl(fd, VIDIOC_G_FMT, &format) != 0) goto fail;
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 ||
        format.fmt.pix.width != k_width[resolution] || format.fmt.pix.height != k_height[resolution]) goto fail;

    struct v4l2_requestbuffers req = {.count = 3, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2 || req.count > 3) goto fail;
    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) goto fail;
        lengths[i] = buf.length;
        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (!buffers[i] || buffers[i] == MAP_FAILED) { buffers[i] = NULL; goto fail; }
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) goto fail;
    }
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) goto fail;
    streaming = true;
    ESP_LOGI(TAG, "preview started: %s RGB565", camera_res_name(resolution));
    int64_t last_display = 0, last_jpeg = 0;
    unsigned frames = 0;
    while (capture_requested() && atomic_load(&s_resolution) == resolution) {
        struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) goto fail;
        size_t expected = (size_t)k_width[resolution] * k_height[resolution] * 2;
        if (buf.index >= req.count || lengths[buf.index] < expected || buf.bytesused < expected) goto fail;
        int64_t now = esp_timer_get_time();
        if (now - last_display >= 100000 && atomic_load(&s_active)) {
            ui_camera_frame(buffers[buf.index], k_width[resolution], k_height[resolution]);
            last_display = now;
            if (frames++ == 0) ESP_LOGI(TAG, "preview received first frame (%u bytes)", (unsigned)buf.bytesused);
        }
        if (atomic_load(&s_stream_clients) && now - last_jpeg >= 1000000 / atomic_load(&s_fps)) {
            encode_frame(buffers[buf.index], k_width[resolution], k_height[resolution]);
            last_jpeg = now;
        }
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) goto fail;
    }
    goto cleanup;
fail:
    ESP_LOGE(TAG, "preview failed at %s (errno %d)", camera_res_name(resolution), errno);
    ui_camera_status("Camera unavailable. Reopen Camera to retry.");
    /* Wait for a new UI action instead of restarting a failed pipeline in a loop. */
    atomic_store(&s_active, false);
    atomic_store(&s_fault, true);
cleanup:
    if (streaming) ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < 3; i++) if (buffers[i]) munmap(buffers[i], lengths[i]);
    if (fd >= 0) close(fd);
    if (streaming) ESP_LOGI(TAG, "preview stopped");
}

static void camera_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "checking removable camera at boot");
    esp_err_t err = probe_camera();
    if (err == ESP_OK) {
        err = init_sensor();
        /* esp_video_init can succeed without finding a camera sensor. */
        if (err == ESP_OK && !esp_video_get_csi_video_device_sensor()) err = ESP_ERR_NOT_FOUND;
    }
    bool available = err == ESP_OK;
    atomic_store(&s_available, available);
    ui_camera_availability(available);
    if (!available) {
        ESP_LOGW(TAG, "camera unavailable at boot (%s); dashboard continues normally", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SC2336 ready on SCCB GPIO12/13");
    }
    for (;;) {
        if (available && capture_requested()) preview_run(atomic_load(&s_resolution));
        else ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

void camera_init(void)
{
    s_jpeg_mutex = xSemaphoreCreateMutex(); configASSERT(s_jpeg_mutex);
    camera_apply();
    atomic_store(&s_resolution, g_cfg.cam_res < CAMERA_RES_COUNT ? g_cfg.cam_res : CAMERA_RES_DEFAULT);
    if (xTaskCreate(camera_task, "camera", 6144, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ui_camera_availability(false);
    }
}

void camera_set_active(bool active)
{
    atomic_store(&s_active, active);
    if (active) atomic_store(&s_fault, false);
    if (s_task) xTaskNotifyGive(s_task);
}

void camera_apply(void)
{
    settings_lock();
    atomic_store(&s_resolution, g_cfg.cam_res < CAMERA_RES_COUNT ? g_cfg.cam_res : CAMERA_RES_DEFAULT);
    atomic_store(&s_fps, g_cfg.stream_fps < 1 || g_cfg.stream_fps > 15 ? 5 : g_cfg.stream_fps);
    atomic_store(&s_quality, g_cfg.stream_quality < 20 || g_cfg.stream_quality > 90 ? 70 : g_cfg.stream_quality);
    settings_unlock();
    atomic_store(&s_fault, false);
    if (s_task) xTaskNotifyGive(s_task);
}
