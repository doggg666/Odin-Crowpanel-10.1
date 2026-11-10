#include "board.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "sdkconfig.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "board";

static esp_ldo_channel_handle_t s_ldo3, s_ldo4;
static i2c_master_bus_handle_t s_i2c;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_dsi_bus_handle_t s_dsi;
static esp_lcd_panel_io_handle_t s_dbi;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static uint8_t s_volume = 70;
static bool s_audio_playing;

/* CrowPanel v1.0 amplifier shutdown: low enables sound, high mutes. */
static void audio_update_amp(void)
{
    gpio_set_level(BOARD_AMP_GPIO, !s_audio_playing || s_volume == 0);
}

/* ------------------------------------------------------------------ power */
static esp_err_t power_init(void)
{
    esp_ldo_channel_config_t l3 = { .chan_id = 3, .voltage_mv = 2500 };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&l3, &s_ldo3), TAG, "ldo3");
    esp_ldo_channel_config_t l4 = { .chan_id = 4, .voltage_mv = 3300 };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&l4, &s_ldo4), TAG, "ldo4");
    return ESP_OK;
}

/* -------------------------------------------------------------------- i2c */
static esp_err_t i2c_init_bus(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

/* ------------------------------------------------------------------ touch */
static esp_err_t touch_init(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = { .disable_control_phase = 1 },
        .scl_speed_hz = 400000,
    };
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_H_RES,
        .y_max = BOARD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_RST,
        .int_gpio_num = BOARD_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    tp_cfg.driver_data = (void *)&io_cfg.dev_addr;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch io");
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        /* GT911 can come up on its backup address depending on INT level at reset */
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch io2");
        err = esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch);
    }
    return err;
}

/* -------------------------------------------------------------- backlight */
static esp_err_t blight_init(void)
{
    const ledc_timer_config_t t = {
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .freq_hz = 30000,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
    };
    const ledc_channel_config_t c = {
        .gpio_num = BOARD_BLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");
    return ledc_channel_config(&c);
}

void board_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t duty = pct ? (uint32_t)pct * 18 + 200 : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------------------------------------------------------------- display */
static esp_err_t panel_init(void)
{
    esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 900,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus, &s_dsi), TAG, "dsi bus");

    esp_lcd_dbi_io_config_t dbi = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi, &dbi, &s_dbi), TAG, "dbi io");

    const esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 51,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_H_RES,
            .v_size = BOARD_V_RES,
            .hsync_back_porch = 160,
            .hsync_pulse_width = 70,
            .hsync_front_porch = 160,
            .vsync_back_porch = 23,
            .vsync_pulse_width = 10,
            .vsync_front_porch = 12,
        },
        .flags.use_dma2d = true,
    };
    ek79007_vendor_config_t vendor = { .mipi_config = { .dsi_bus = s_dsi, .dpi_config = &dpi } };
    const esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(s_dbi, &pcfg, &s_panel), TAG, "ek79007");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    return esp_lcd_panel_init(s_panel);
}

static esp_err_t lvgl_setup(void)
{
    const lvgl_port_cfg_t lcfg = {
        .task_priority = configMAX_PRIORITIES - 4,
        .task_stack = 8192 * 2,
        .task_affinity = -1,
        .task_max_sleep_ms = 10,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lcfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp = {
        .io_handle = s_dbi,
        .panel_handle = s_panel,
        .control_handle = s_panel,
        .buffer_size = BOARD_H_RES * BOARD_V_RES * 2,
        .double_buffer = true,
        .hres = BOARD_H_RES,
        .vres = BOARD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = { .flags = { .avoid_tearing = false } };
    s_disp = lvgl_port_add_disp_dsi(&disp, &dsi_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "add display");

    const lvgl_port_touch_cfg_t tcfg = { .disp = s_disp, .handle = s_touch };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&tcfg), ESP_FAIL, TAG, "add touch");
    return ESP_OK;
}

/* ------------------------------------------------------------------- misc */

/* --- I2S speaker output (GPIO23 DOUT / 22 BCLK / 21 LRCLK) --- */
static i2s_chan_handle_t s_i2s;
static int s_i2s_rate;

static esp_err_t audio_open_rate(int rate)
{
    static const i2s_std_gpio_config_t i2s_gpios = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = 22,
        .ws   = 21,
        .dout = 23,
        .din  = I2S_GPIO_UNUSED,
    };
    /* Match CrowPanel ref: STEREO/BOTH, ws_width 16, bit_shift+left_align (DAC/amp expects LR framing) */
    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    slot.slot_mask = I2S_STD_SLOT_BOTH;
    slot.ws_width = 16;
    slot.bit_shift = true;
    slot.left_align = true;

    if (!s_i2s) {
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        cc.auto_clear = true;
        cc.dma_desc_num = 8;
        cc.dma_frame_num = 240;
        ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_i2s, NULL), TAG, "i2s chan");

        i2s_std_config_t sc = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate),
            .slot_cfg = slot,
            .gpio_cfg = i2s_gpios,
        };
        esp_err_t e = i2s_channel_init_std_mode(s_i2s, &sc);
        if (e != ESP_OK) return e;
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s), TAG, "i2s en");
        s_i2s_rate = rate;
        return ESP_OK;
    }

    if (rate != s_i2s_rate) {
        i2s_std_config_t sc = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate),
            .slot_cfg = slot,
            .gpio_cfg = i2s_gpios,
        };
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_i2s), TAG, "i2s dis");
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s, &sc.clk_cfg), TAG, "i2s clk");
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_i2s, &sc.slot_cfg), TAG, "i2s slot");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s), TAG, "i2s en");
        s_i2s_rate = rate;
    }
    return ESP_OK;
}

/* --- PDM microphone input (GPIO24 CLK / GPIO26 DATA, 16 kHz mono PCM) --- */
static i2s_chan_handle_t s_mic;

esp_err_t board_mic_init(void)
{
    if (s_mic) return ESP_OK;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = 512;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, NULL, &s_mic), TAG, "mic chan");

    i2s_pdm_rx_config_t pcfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = 24,
            .din = 26,
            .invert_flags = { .clk_inv = false },
        },
    };
    esp_err_t e = i2s_channel_init_pdm_rx_mode(s_mic, &pcfg);
    if (e != ESP_OK) {
        i2s_del_channel(s_mic);
        s_mic = NULL;
        return e;
    }
    /* leave disabled until capture to avoid running RX DMA continuously */
    ESP_LOGI(TAG, "PDM mic inited (GPIO24 clk / GPIO26 data @ 16 kHz)");
    return ESP_OK;
}

esp_err_t board_mic_start(void)
{
    if (!s_mic) return ESP_ERR_INVALID_STATE;
    esp_err_t e = i2s_channel_enable(s_mic);
    return (e == ESP_OK || e == ESP_ERR_INVALID_STATE) ? ESP_OK : e;
}
void board_mic_stop(void)
{
    if (s_mic) i2s_channel_disable(s_mic);
}

int board_mic_read(int16_t *dst, size_t samples, uint32_t timeout_ms)
{
    if (!s_mic || !dst || !samples) return -1;
    size_t bytes = 0;
    esp_err_t e = i2s_channel_read(s_mic, dst, samples * sizeof(int16_t), &bytes, timeout_ms);
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) return -1;
    return (int)(bytes / sizeof(int16_t));
}

esp_err_t board_audio_init(void)
{
    return audio_open_rate(24000);
}

bool board_audio_play(const int16_t *pcm, size_t frames, int rate, bool (*cancelled)(void))
{
    if (!pcm || !frames || rate <= 0) return false;
    if (audio_open_rate(rate) != ESP_OK) return false;

    s_audio_playing = true;
    audio_update_amp();
    ESP_LOGI(TAG, "speaker: %u frames @ %d Hz, volume %u%%", (unsigned)frames, rate, s_volume);
    const size_t CHUNK = 240; /* mono frames per chunk -> 480 stereo samples */
    int16_t scratch[CHUNK * 2];
    size_t done = 0;

    while (done < frames && !(cancelled && cancelled())) {
        size_t n = frames - done;
        if (n > CHUNK) n = CHUNK;
        const float gain = s_volume / 100.0f;
        for (size_t i = 0; i < n; i++) {
            int v = (int)((float)pcm[done + i] * gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            int16_t s = (int16_t)v;
            scratch[i * 2] = s;       /* L */
            scratch[i * 2 + 1] = s;   /* R (duplicate mono to both slots) */
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_i2s, scratch, n * 2 * sizeof(int16_t), &written, 100);
        if (err != ESP_OK || written != n * 2 * sizeof(int16_t)) {
            ESP_LOGW(TAG, "speaker write failed: %s (%u bytes)", esp_err_to_name(err), (unsigned)written);
            break;
        }
        done += n;
    }
    /* Drain eight DMA buffers before muting the final syllable. */
    if (done == frames) {
        int drain_ms = (8 * 240 * 1000 + rate - 1) / rate + 10;
        for (int elapsed = 0; elapsed < drain_ms && !(cancelled && cancelled()); elapsed += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    s_audio_playing = false;
    audio_update_amp();
    ESP_LOGI(TAG, "speaker: played %u/%u frames", (unsigned)done, (unsigned)frames);
    return done == frames && !(cancelled && cancelled());
}

void board_set_volume(uint8_t pct)
{
    s_volume = pct > 100 ? 100 : pct;
    audio_update_amp();
}
uint8_t board_get_volume(void) { return s_volume; }

/* ---------------------------------------------------------------- battery
 * Read from the on-board STC8H1Kxx power MCU over I2C (addr 0x2F, reg 0x00).
 * Battery_info_t layout (11 bytes, little-endian):
 *   u32 adc_voltage, u32 bat_voltage(mV), u8 bat_level(%), u8 bat_state, u8 led_state
 * If bat_voltage > 4300 mV the board is on USB/external power (return -2). */
#define STC8_ADDR        0x2F
#define STC8_REG_BATTERY 0x00

typedef struct __attribute__((packed)) {
    uint32_t adc_voltage;
    uint32_t bat_voltage;
    uint8_t  bat_level;
    uint8_t  bat_state;
    uint8_t  led_state;
} stc8_bat_t;

static i2c_master_dev_handle_t s_stc8;

static void stc8_init_device(void)
{
    if (!s_i2c || s_stc8) return;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = STC8_ADDR,
        .scl_speed_hz    = 100000, /* conservative; STC8 is reliable at 100k */
    };
    i2c_master_bus_add_device(s_i2c, &cfg, &s_stc8);
}

static esp_err_t stc8_read_battery(stc8_bat_t *out)
{
    if (!s_stc8) stc8_init_device();
    if (!s_stc8) return ESP_ERR_INVALID_STATE;
    /* The STC8 does not auto-increment its register on a bulk read. */
    for (uint8_t i = 0; i < sizeof(*out); i++) {
        uint8_t reg = STC8_REG_BATTERY + i;
        esp_err_t err = i2c_master_transmit_receive(s_stc8, &reg, 1, (uint8_t *)out + i, 1, 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

int board_battery_percent(int *millivolts)
{
    if (millivolts) *millivolts = 0;
    stc8_bat_t info = {0};
    esp_err_t err = stc8_read_battery(&info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
        return -1;
    }
    if (info.adc_voltage > 3600 || info.bat_voltage < 2000 || info.bat_voltage > 5500 ||
        info.bat_level > 100 || info.bat_state > 4 || info.led_state > 4) {
        ESP_LOGW(TAG, "invalid battery data: ADC %lu, voltage %lu, level %u, state %u",
                 (unsigned long)info.adc_voltage, (unsigned long)info.bat_voltage, info.bat_level, info.bat_state);
        return -1;
    }
    if (millivolts) *millivolts = (int)info.bat_voltage;
    ESP_LOGI(TAG, "battery: %lu mV, %u%%, charge state %u",
             (unsigned long)info.bat_voltage, info.bat_level, info.bat_state);
    if (info.bat_voltage > 4300) return -2; /* USB connected / external power (> 4.3 V) */
    int pct = info.bat_level;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(power_init(), TAG, "power");
    ESP_RETURN_ON_ERROR(i2c_init_bus(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");
    ESP_RETURN_ON_ERROR(blight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");
    ESP_RETURN_ON_ERROR(lvgl_setup(), TAG, "lvgl");

    const gpio_config_t amp = {
        .pin_bit_mask = 1ULL << BOARD_AMP_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp);
    audio_update_amp();

    if (board_mic_init() != ESP_OK) {
        ESP_LOGW(TAG, "PDM mic init failed (GPIO24/26) - mic will be unavailable");
    }
    stc8_init_device(); /* battery/USB status via STC8 MCU */

    board_set_brightness(0);
    ESP_LOGI(TAG, "display and touch ready");
    return ESP_OK;
}
