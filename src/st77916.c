/*
   +----------------------------------------------------------------------+
   | php-baremetal: esp32-display-st77916                                 |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/* ST77916 QSPI panel over esp_lcd: fill and solid rectangles. The panel IO runs on the SPI host of the
 * Baremetal\Spi\Bus. */
#include "php_spi.h"
#include "bm_spi_hal.h"
#include "esp_lcd_st77916.h"
#include "st77916_init_cmds.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "zend_exceptions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define FLUSH_BAND      40          /* rows per DMA band at full width */
#define IO_HZ           (40 * 1000 * 1000)
#define ID_READ_HZ      (5 * 1000 * 1000)
#define DONE_TIMEOUT_MS 1000
#define DRAIN_TIMEOUT_MS 1000

extern const spi_driver_desc_t st77916_driver_desc;

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t done;          /* given once per finished color transfer */
    uint16_t *band;                  /* DMA-capable internal buffer, width * FLUSH_BAND pixels */
    size_t band_px;
    int  width, height;
    int  x_gap, y_gap;
    int  rst, bl;
    bool mirror_x, mirror_y, swap_xy, invert;
    bool inited;
    bool failed;                     /* a transfer timed out and could not be drained */
} st_state;

static inline uint16_t sw(uint16_t c) { return (uint16_t) ((c >> 8) | (c << 8)); }

static IRAM_ATTR bool st_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev,
                                    void *ctx)
{
    (void) io;
    (void) ev;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t) ctx, &woken);
    return woken == pdTRUE;
}

static void st_reset_pulse(st_state *s)
{
    if (s->rst < 0) {
        return;
    }
    gpio_set_level(s->rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t st_configure(st_state *s)
{
    esp_err_t e = ESP_OK;
    if (s->swap_xy && (e = esp_lcd_panel_swap_xy(s->panel, true)) != ESP_OK) {
        return e;
    }
    if ((s->mirror_x || s->mirror_y) && (e = esp_lcd_panel_mirror(s->panel, s->mirror_x, s->mirror_y)) != ESP_OK) {
        return e;
    }
    if ((s->x_gap || s->y_gap) && (e = esp_lcd_panel_set_gap(s->panel, s->x_gap, s->y_gap)) != ESP_OK) {
        return e;
    }
    if (s->invert && (e = esp_lcd_panel_invert_color(s->panel, true)) != ESP_OK) {
        return e;
    }
    return esp_lcd_panel_disp_on_off(s->panel, true);
}

static void st_release(st_state *s)
{
    if (s->panel) {
        esp_lcd_panel_del(s->panel);
        s->panel = NULL;
    }
    if (s->io) {
        esp_lcd_panel_io_del(s->io);
        s->io = NULL;
    }
    if (s->band) {
        heap_caps_free(s->band);
        s->band = NULL;
    }
    if (s->done) {
        vSemaphoreDelete(s->done);
        s->done = NULL;
    }
}

/* Drive the backlight pin high (no-op without one). */
static void st_backlight_on(st_state *s)
{
    if (s->bl >= 0) {
        gpio_reset_pin(s->bl);
        gpio_set_direction(s->bl, GPIO_MODE_OUTPUT);
        gpio_set_level(s->bl, 1);
    }
}

static esp_err_t st_hw_init(st_state *s, int host, int cs)
{
    s->band_px = (size_t) s->width * FLUSH_BAND;
    s->band = heap_caps_malloc(s->band_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s->done = xSemaphoreCreateCounting(64, 0);
    if (!s->band || !s->done) {
        st_release(s);
        return ESP_ERR_NO_MEM;
    }

    if (s->rst >= 0) {
        gpio_reset_pin(s->rst);
        gpio_set_direction(s->rst, GPIO_MODE_OUTPUT);
    }
    st_reset_pulse(s);

    /* read register 0x04 at a low clock to pick the panel's init variant */
    esp_lcd_spi_bus_handle_t bus = (esp_lcd_spi_bus_handle_t) host;
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(cs, NULL, NULL);
    io_cfg.pclk_hz = ID_READ_HZ;
    esp_err_t e = esp_lcd_new_panel_io_spi(bus, &io_cfg, &s->io);
    if (e != ESP_OK) {
        st_release(s);
        return e;
    }
    uint8_t rd[4] = {0};
    int read_cmd = (int) ((0x0BUL << 24) | (0x04UL << 8));
    esp_lcd_panel_io_rx_param(s->io, read_cmd, rd, sizeof(rd));
    esp_lcd_panel_io_del(s->io);
    s->io = NULL;

    esp_lcd_panel_io_spi_config_t io_fast = ST77916_PANEL_IO_QSPI_CONFIG(cs, st_color_done, s->done);
    io_fast.pclk_hz = IO_HZ;
    e = esp_lcd_new_panel_io_spi(bus, &io_fast, &s->io);
    if (e != ESP_OK) {
        st_release(s);
        return e;
    }

    st77916_vendor_config_t vendor = { .flags = { .use_qspi_interface = 1 } };
    if (rd[0] == 0x00 && rd[1] == 0x02 && rd[2] == 0x7F && rd[3] == 0x7F) {
        vendor.init_cmds = vendor_specific_init_version_2;
        vendor.init_cmds_size = sizeof(vendor_specific_init_version_2) / sizeof(st77916_lcd_init_cmd_t);
    } else {
        vendor.init_cmds = vendor_specific_init_version_1;
        vendor.init_cmds_size = sizeof(vendor_specific_init_version_1) / sizeof(st77916_lcd_init_cmd_t);
    }
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,   /* pulsed above */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    e = esp_lcd_new_panel_st77916(s->io, &dev, &s->panel);
    if (e != ESP_OK) {
        s->panel = NULL;
        st_release(s);
        return e;
    }
    e = esp_lcd_panel_reset(s->panel);
    if (e == ESP_OK) {
        e = esp_lcd_panel_init(s->panel);
    }
    if (e == ESP_OK) {
        e = st_configure(s);
    }
    if (e != ESP_OK) {
        st_release(s);
        return e;
    }

    st_backlight_on(s);
    return ESP_OK;
}

/* Paint [x, x+w) x [y, y+h) with one RGB565 colour from the band buffer, then wait for the DMA. */
static esp_err_t st_fill_rect(st_state *s, int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > s->width) {
        w = s->width - x;
    }
    if (y + h > s->height) {
        h = s->height - y;
    }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    int rows = (int) (s->band_px / (size_t) w);
    size_t n = (size_t) w * (size_t) rows;
    uint16_t swc = sw(color);
    for (size_t i = 0; i < n; i++) {
        s->band[i] = swc;
    }

    xQueueReset(s->done);
    esp_err_t ret = ESP_OK;
    int queued = 0;
    for (int yy = y; yy < y + h; yy += rows) {
        int hh = (yy + rows <= y + h) ? rows : (y + h - yy);
        esp_err_t e = esp_lcd_panel_draw_bitmap(s->panel, x, yy, x + w, yy + hh, s->band);
        if (e != ESP_OK) {
            ret = e;
            break;
        }
        queued++;
    }
    for (int i = 0; i < queued; i++) {
        if (xSemaphoreTake(s->done, pdMS_TO_TICKS(DONE_TIMEOUT_MS)) != pdTRUE) {
            ret = ESP_ERR_TIMEOUT;
            /* drain what is still in flight so no queued transfer or stale give outlives this call */
            for (int j = i + 1; j < queued; j++) {
                if (xSemaphoreTake(s->done, pdMS_TO_TICKS(DRAIN_TIMEOUT_MS)) != pdTRUE) {
                    s->failed = true;
                    break;
                }
            }
            /* the band that timed out may still complete later */
            if (xSemaphoreTake(s->done, pdMS_TO_TICKS(DRAIN_TIMEOUT_MS)) != pdTRUE) {
                s->failed = true;
            }
            break;
        }
    }
    return ret;
}

static st_state *this_state(zval *zthis)
{
    spi_dev_t *d = spi_device_this(zthis);
    if (!d) {
        return NULL;
    }
    st_state *s = d->state;
    if (!s || !s->inited) {
        zend_throw_exception(zend_ce_exception, "st77916 is not initialised", 0);
        return NULL;
    }
    if (s->failed) {
        zend_throw_exception(zend_ce_exception, "st77916 failed: a display transfer never completed", 0);
        return NULL;
    }
    return s;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_st_ctor, 0, 0, 1)
    ZEND_ARG_OBJ_INFO(0, bus, Baremetal\\Spi\\Bus, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cs, IS_LONG, 0, "21")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, rst, IS_LONG, 0, "3")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bl, IS_LONG, 0, "-1")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, width, IS_LONG, 0, "360")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, height, IS_LONG, 0, "360")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, x_gap, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, y_gap, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mirror_x, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mirror_y, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, swap_xy, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, invert, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_st_fill, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_st_rect, 0, 5, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, h, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_st_rgb, 0, 3, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, r, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, g, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, b, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_st_wake, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(St77916, __construct)
{
    zval *zbus;
    zend_long cs = 21, rst = 3, bl = -1, width = 360, height = 360, x_gap = 0, y_gap = 0;
    bool mirror_x = 0, mirror_y = 0, swap_xy = 0, invert = 0;
    ZEND_PARSE_PARAMETERS_START(1, 12)
        Z_PARAM_OBJECT_OF_CLASS(zbus, spi_bus_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(cs)
        Z_PARAM_LONG(rst)
        Z_PARAM_LONG(bl)
        Z_PARAM_LONG(width)
        Z_PARAM_LONG(height)
        Z_PARAM_LONG(x_gap)
        Z_PARAM_LONG(y_gap)
        Z_PARAM_BOOL(mirror_x)
        Z_PARAM_BOOL(mirror_y)
        Z_PARAM_BOOL(swap_xy)
        Z_PARAM_BOOL(invert)
    ZEND_PARSE_PARAMETERS_END();

    if (!spi_check_device_args(2, cs, 0, 0, 0, 0)) {   /* the same CS range as ext-spi's constructors */
        RETURN_THROWS();
    }
    if (rst < -1 || rst > 255 || bl < -1 || bl > 255) {
        zend_argument_value_error((rst < -1 || rst > 255) ? 3 : 4, "must be a GPIO number or -1");
        RETURN_THROWS();
    }
    if (width < 1 || width > 4096 || height < 1 || height > 4096) {
        zend_argument_value_error((width < 1 || width > 4096) ? 5 : 6, "must be between 1 and 4096");
        RETURN_THROWS();
    }
    spi_bus_object *bo = spi_bus_object_from(Z_OBJ_P(zbus));
    if (!bo->bus) {
        zend_throw_exception(zend_ce_exception, "Spi\\Bus is not initialised", 0);
        RETURN_THROWS();
    }

    bm_err_t err = BM_OK;
    spi_dev_t *dev = spi_dev_get(bo->bus, (int) cs, st77916_driver_desc.default_hz,
                                 st77916_driver_desc.default_mode, &st77916_driver_desc, &err);
    if (!dev) {
        zend_throw_exception_ex(zend_ce_exception, 0, "cannot reserve st77916 (cs=%d): %s", (int) cs,
                                bm_spi_hal_err_name(err));
        RETURN_THROWS();
    }
    st_state *s = dev->state;
    if (s->inited) {
        if (s->rst != (int) rst || s->bl != (int) bl || s->width != (int) width || s->height != (int) height ||
            s->x_gap != (int) x_gap || s->y_gap != (int) y_gap || s->mirror_x != mirror_x ||
            s->mirror_y != mirror_y || s->swap_xy != swap_xy || s->invert != invert) {
            zend_throw_exception_ex(zend_ce_exception, 0,
                                    "display on cs %d already initialised with different parameters", (int) cs);
            RETURN_THROWS();
        }
    }
    spi_device_object_from(Z_OBJ_P(ZEND_THIS))->dev = dev;
    if (!s->inited) {
        s->rst = (int) rst;
        s->bl = (int) bl;
        s->width = (int) width;
        s->height = (int) height;
        s->x_gap = (int) x_gap;
        s->y_gap = (int) y_gap;
        s->mirror_x = mirror_x;
        s->mirror_y = mirror_y;
        s->swap_xy = swap_xy;
        s->invert = invert;
        esp_err_t e = st_hw_init(s, bo->bus->host, (int) cs);
        if (e != ESP_OK) {
            zend_throw_exception_ex(zend_ce_exception, 0, "st77916 init failed: %s", esp_err_to_name(e));
            RETURN_THROWS();
        }
        s->inited = true;
    }
}

/* fill(int color): paint the whole panel one RGB565 colour, band by band. */
PHP_METHOD(St77916, fill)
{
    zend_long color;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(color)
    ZEND_PARSE_PARAMETERS_END();
    st_state *s = this_state(ZEND_THIS);
    if (!s) {
        RETURN_THROWS();
    }
    esp_err_t e = st_fill_rect(s, 0, 0, s->width, s->height, (uint16_t) color);
    if (e != ESP_OK) {
        zend_throw_exception_ex(zend_ce_exception, 0, "st77916 fill failed: %s", esp_err_to_name(e));
        RETURN_THROWS();
    }
}

/* rect(x, y, w, h, color): paint a solid rectangle, clipped to the panel. */
PHP_METHOD(St77916, rect)
{
    zend_long x, y, w, h, color;
    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_LONG(w)
        Z_PARAM_LONG(h)
        Z_PARAM_LONG(color)
    ZEND_PARSE_PARAMETERS_END();
    st_state *s = this_state(ZEND_THIS);
    if (!s) {
        RETURN_THROWS();
    }
    if (x < -s->width || x > s->width || y < -s->height || y > s->height ||
        w < 0 || w > 2 * s->width || h < 0 || h > 2 * s->height) {
        return;   /* out of the panel: nothing to paint (also keeps the int maths in range) */
    }
    esp_err_t e = st_fill_rect(s, (int) x, (int) y, (int) w, (int) h, (uint16_t) color);
    if (e != ESP_OK) {
        zend_throw_exception_ex(zend_ce_exception, 0, "st77916 rect failed: %s", esp_err_to_name(e));
        RETURN_THROWS();
    }
}

PHP_METHOD(St77916, rgb)
{
    zend_long r, g, b;
    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_LONG(r) Z_PARAM_LONG(g) Z_PARAM_LONG(b)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
}

/* wake(): re-init the panel after a light sleep that disturbed the QSPI lines (then repaint). */
PHP_METHOD(St77916, wake)
{
    ZEND_PARSE_PARAMETERS_NONE();
    st_state *s = this_state(ZEND_THIS);
    if (!s) {
        RETURN_THROWS();
    }
    st_reset_pulse(s);
    esp_err_t e = esp_lcd_panel_init(s->panel);
    if (e == ESP_OK) {
        e = st_configure(s);
    }
    if (e != ESP_OK) {
        zend_throw_exception_ex(zend_ce_exception, 0, "st77916 wake failed: %s", esp_err_to_name(e));
        RETURN_THROWS();
    }
    st_backlight_on(s);   /* the sleep may have released the pin too */
}

static const zend_function_entry st77916_methods[] = {
    PHP_ME(St77916, __construct, arginfo_st_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(St77916, fill,        arginfo_st_fill, ZEND_ACC_PUBLIC)
    PHP_ME(St77916, rect,        arginfo_st_rect, ZEND_ACC_PUBLIC)
    PHP_ME(St77916, rgb,         arginfo_st_rgb,  ZEND_ACC_PUBLIC)
    PHP_ME(St77916, wake,        arginfo_st_wake, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

const spi_driver_desc_t st77916_driver_desc = {
    .name = "st77916",
    .php_class = "St77916",
    .capability = "Baremetal\\Output\\Display",
    .default_cs = 21,
    .default_hz = IO_HZ,
    .default_mode = 0,
    .managed_io = true,
    .state_size = sizeof(st_state),
    .methods = st77916_methods,
};
