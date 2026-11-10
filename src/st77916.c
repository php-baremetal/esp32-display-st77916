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

/* ST77916 QSPI panel over esp_lcd, a panel of ext-display: bring-up, asynchronous region flush, backlight, sleep.
 * The panel IO runs on the SPI host of the Baremetal\Spi\Bus. A flush task on core 1 sends the regions
 * ext-display begins, through two ping-pong bands: while one is in DMA, the task gathers the next. */
#include "php_spi.h"
#include "bm_spi_hal.h"
#include "bm_board.h"
#include "display_panel.h"
#include "esp_lcd_st77916.h"
#include "st77916_init_cmds.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "ext/spl/spl_exceptions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define FLUSH_ROWS      20          /* rows per ping-pong band at full width */
#define DIRECT_ROWS     60          /* rows per transfer of a direct region, so sent_y moves during a frame */
/* One job in flight per surface, and the constructor lets up to JOB_QUEUE_LEN objects share one panel. begin
 * never blocks: it also runs in the flush task (display_job_done() begins the pending region) and in the TE
 * interrupt, and a blocking send from the flush task onto its own full queue would deadlock it. */
#define JOB_QUEUE_LEN   4
/* Bands of the band path accepted and not yet done (the contract asks for at least 2). They share the job queue with
 * the flush jobs, which is sized for both: a band takes a slot first, so its send never finds the queue full. */
#define BAND_SLOTS      3
/* push blocks this long for a free slot before it gives up with ESP_ERR_TIMEOUT: a deliberate bound on the contract's
 * "blocks until it can", against a stuck task. */
#define BAND_ACCEPT_TIMEOUT_MS 5000
#define TASK_STACK      3072
#define TASK_CORE       1
#define IO_HZ           (40 * 1000 * 1000)
#define ID_READ_HZ      (5 * 1000 * 1000)
#define DONE_TIMEOUT_MS 1000
#define DRAIN_TIMEOUT_MS 1000

extern const spi_driver_desc_t st77916_driver_desc;

/* What the flush task takes from its queue: a region of the surface (display_job_t), a band of the band path, or
 * the stop request. */
typedef enum { ST_JOB_FLUSH, ST_JOB_BAND, ST_JOB_STOP } st_job_kind_t;

typedef struct {
    st_job_kind_t kind;
    display_job_t flush;               /* ST_JOB_FLUSH */
    struct {                           /* ST_JOB_BAND */
        int x0, x1, y0, rows;
        const void *px;
        void (*done)(void *arg);
        void *arg;
    } band;
} st_job_t;

/* The panel's nominal refresh rate with the init sequences of this driver; not measured. */
#define ST_REFRESH_HZ 60.0f

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t done;          /* given once per finished color transfer */
    uint16_t *band[2];               /* DMA-capable internal, width * FLUSH_ROWS pixels each; at the first region
                                      * that needs them (st_bands), never on the band path */
    size_t band_px;
    QueueHandle_t jobs;              /* JOB_QUEUE_LEN + BAND_SLOTS st_job_t */
    SemaphoreHandle_t slots;         /* counting, BAND_SLOTS: a band holds one from push until after its done */
    bool band_open;                  /* band path: begin() was called and end() not yet (the renderer's task only) */
    int  band_x0, band_y0, band_x1, band_y1;
    SemaphoreHandle_t progress;      /* binary, given after each band and after each job */
    SemaphoreHandle_t gone;          /* given by the task when it ends */
    TaskHandle_t task;
    int  te;                         /* GPIO of the panel's TE output, -1 for none; ext-display owns it */
    display_panel_ops_t ops;
    display_band_ops_t band_ops;     /* st_band_ops with this panel's scan order */
    int  width, height;
    int  x_gap, y_gap;
    int  rst, bl;
    bool mirror_x, mirror_y, swap_xy, invert;
    bool buffered, vsync;            /* the options the surface was attached with */
    bool inited;
    bool failed;                     /* a transfer timed out and could not be drained */
    bool bl_on;                      /* the backlight level is above 0 */
    bool bl_ready;                   /* the backlight pin is configured as an output */
} st_state;

/* The DMA of a color transfer ended (SPI interrupt). esp_lcd's SPI IO drops this callback's return value, so the
 * switch to the flush task is asked for here: without it the task would only run at the next tick (up to 1 ms
 * later), and every band would wait for it. */
static IRAM_ATTR bool st_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev,
                                    void *ctx)
{
    (void) io;
    (void) ev;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t) ctx, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
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

/* Stops the flush task, once no surface of this panel has a flush in flight or pending. */
static void st_task_stop(st_state *s)
{
    if (!s->task) {
        return;
    }
    display_quiesce(s);
    st_job_t stop = { .kind = ST_JOB_STOP };
    xQueueSend(s->jobs, &stop, portMAX_DELAY);
    xSemaphoreTake(s->gone, portMAX_DELAY);
    s->task = NULL;
}

static void st_release(st_state *s)
{
    st_task_stop(s);
    if (s->panel) {
        esp_lcd_panel_del(s->panel);
        s->panel = NULL;
    }
    if (s->io) {
        esp_lcd_panel_io_del(s->io);
        s->io = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (s->band[i]) {
            heap_caps_free(s->band[i]);
            s->band[i] = NULL;
        }
    }
    if (s->jobs) {
        vQueueDelete(s->jobs);
        s->jobs = NULL;
    }
    SemaphoreHandle_t *sems[] = { &s->done, &s->progress, &s->gone, &s->slots };
    for (size_t i = 0; i < sizeof sems / sizeof sems[0]; i++) {
        if (*sems[i]) {
            vSemaphoreDelete(*sems[i]);
            *sems[i] = NULL;
        }
    }
}

/* Drive the backlight pin (no-op without one). The pin is configured on first use and again after a
 * re-initialisation, as a light sleep may release it. */
static void st_backlight_set(st_state *s, bool on)
{
    if (s->bl < 0) {
        return;
    }
    if (!s->bl_ready) {
        gpio_reset_pin(s->bl);
        gpio_set_direction(s->bl, GPIO_MODE_OUTPUT);
        s->bl_ready = true;
    }
    gpio_set_level(s->bl, on ? 1 : 0);
}

/* One transfer is out: wait for its DMA (the done semaphore). A transfer that times out may still complete
 * later, so it is waited for once more before its buffer is reused; if it never ends the panel is failed. */
static bm_err_t st_band_done(st_state *s)
{
    if (xSemaphoreTake(s->done, pdMS_TO_TICKS(DONE_TIMEOUT_MS)) != pdTRUE) {
        if (xSemaphoreTake(s->done, pdMS_TO_TICKS(DRAIN_TIMEOUT_MS)) != pdTRUE) {
            s->failed = true;
        }
        return ESP_ERR_TIMEOUT;
    }
    return BM_OK;
}

/* The ping-pong pair, allocated by the first region that goes through it: a renderer on the band path (Slate) never
 * flushes the surface, and 2 x 14 400 bytes of internal RAM held from the constructor would sit unused. Called by the
 * flush task, or by st_push() while no job is in flight; kept until the panel is released. */
static bm_err_t st_bands(st_state *s)
{
    for (int i = 0; i < 2; i++) {
        if (!s->band[i]) {
            s->band[i] = heap_caps_malloc(s->band_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (!s->band[i]) {
                return ESP_ERR_NO_MEM;
            }
        }
    }
    return BM_OK;
}

/* A direct region goes out from its source rows as they are, when the DMA can read them. */
static bool st_job_direct(const display_job_t *j)
{
    const uint16_t *src = j->src + (size_t) j->y0 * (size_t) j->stride + (size_t) j->x0;
    return j->direct && esp_ptr_dma_capable(src) && ((uintptr_t) src & 3u) == 0 &&
           ((size_t) j->stride * sizeof(uint16_t) & 3u) == 0;
}

/* Sends one job. Direct: DIRECT_ROWS rows per transfer straight from the source. Otherwise: bands of the
 * ping-pong pair, the next one gathered (job->read) while the one before is in DMA; the second draw_bitmap only
 * starts after that band's DMA ended, so esp_lcd never waits inside CASET/RASET. */
static bm_err_t st_send_job(st_state *s, const display_job_t *j)
{
    if (s->failed) {
        return ESP_ERR_INVALID_STATE;
    }
    int w = j->x1 - j->x0;
    if (w <= 0 || j->y1 <= j->y0) {
        return BM_OK;
    }
    xQueueReset(s->done);
    if (st_job_direct(j)) {
        for (int y = j->y0; y < j->y1; y += DIRECT_ROWS) {
            int ye = y + DIRECT_ROWS < j->y1 ? y + DIRECT_ROWS : j->y1;
            const uint16_t *src = j->src + (size_t) y * (size_t) j->stride + (size_t) j->x0;
            esp_err_t e = esp_lcd_panel_draw_bitmap(s->panel, j->x0, y, j->x1, ye, src);
            if (e == ESP_OK) {
                e = st_band_done(s);
            }
            if (e != ESP_OK) {
                return e;
            }
            display_rows_sent(j, ye);
            xSemaphoreGive(s->progress);
        }
        return BM_OK;
    }
    bm_err_t be = st_bands(s);
    if (be != BM_OK) {
        return be;
    }
    int rows = (int) (s->band_px / (size_t) w);
    int k = 0, prev_end = -1;
    for (int y = j->y0; y < j->y1; y += rows, k++) {
        int ye = y + rows < j->y1 ? y + rows : j->y1;
        j->read(j, y, ye, (uint8_t *) s->band[k & 1]);   /* overlaps the DMA of the band before */
        if (prev_end >= 0) {
            bm_err_t e = st_band_done(s);
            if (e != BM_OK) {
                return e;
            }
            display_rows_sent(j, prev_end);
            xSemaphoreGive(s->progress);
        }
        esp_err_t e = esp_lcd_panel_draw_bitmap(s->panel, j->x0, y, j->x1, ye, s->band[k & 1]);
        if (e != ESP_OK) {
            return e;   /* nothing of this band went out; the one before was waited for above */
        }
        prev_end = ye;
    }
    bm_err_t e = st_band_done(s);
    if (e == BM_OK) {
        display_rows_sent(j, prev_end);
    }
    return e;
}

/* One band of the band path: the band goes out and its DMA is waited for. The band path never waits for the TE
 * pulse: a renderer paces its frames on onVsync() itself. done(arg) is called either way (a failed transfer or a failed panel included): the
 * push accepted the band. The caller frees the slot after it. */
static void st_send_band(st_state *s, const st_job_t *j)
{
    if (!s->failed) {
        xQueueReset(s->done);
        esp_err_t e = esp_lcd_panel_draw_bitmap(s->panel, j->band.x0, j->band.y0, j->band.x1,
                                                j->band.y0 + j->band.rows, j->band.px);
        if (e == ESP_OK) {
            st_band_done(s);   /* a timeout fails the panel (s->failed) when the DMA never ends */
        }
    }
    if (j->band.done) {
        j->band.done(j->band.arg);
    }
}

/* The flush task: one job at a time from the queue. A region of the surface ends with display_job_done() (which
 * may begin the pending region) and a progress signal once the engine's callbacks returned; a band of the band path
 * ends with its done(), here and never in the DMA interrupt, then frees its slot. */
static void st_flush_task(void *arg)
{
    st_state *s = arg;
    st_job_t j;
    for (;;) {
        if (xQueueReceive(s->jobs, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (j.kind == ST_JOB_STOP) {
            break;
        }
        if (j.kind == ST_JOB_BAND) {
            st_send_band(s, &j);
            xSemaphoreGive(s->slots);
            xSemaphoreGive(s->progress);
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        bm_err_t e = st_send_job(s, &j.flush);
        display_job_done(&j.flush, e, (uint32_t) (esp_timer_get_time() - t0));
        xSemaphoreGive(s->progress);
    }
    xSemaphoreGive(s->gone);
    vTaskDelete(NULL);
}

/* ops->begin: hands a copy of the job to the flush task. From the PHP task, the flush task or the TE interrupt. */
static bool st_begin(void *panel, const display_job_t *job)
{
    st_state *s = panel;
    st_job_t j = { .kind = ST_JOB_FLUSH, .flush = *job };
    if (xPortInIsrContext()) {
        BaseType_t woken = pdFALSE;
        bool ok = xQueueSendFromISR(s->jobs, &j, &woken) == pdTRUE;
        portYIELD_FROM_ISR(woken);
        return ok;
    }
    return xQueueSend(s->jobs, &j, 0) == pdTRUE;
}

/* ops->wait: until a band or a job of this panel ends, or ms pass. */
static bool st_wait(void *panel, uint32_t ms)
{
    st_state *s = panel;
    TickType_t ticks = (TickType_t) ((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);   /* rounded up */
    return xSemaphoreTake(s->progress, ticks) == pdTRUE;
}

static esp_err_t st_hw_init(st_state *s, int host, int cs)
{
    s->band_px = (size_t) s->width * FLUSH_ROWS;   /* the bands themselves: st_bands(), at the first region */
    s->done = xSemaphoreCreateCounting(64, 0);
    s->jobs = xQueueCreate(JOB_QUEUE_LEN + BAND_SLOTS, sizeof(st_job_t));
    s->slots = xSemaphoreCreateCounting(BAND_SLOTS, BAND_SLOTS);
    s->progress = xSemaphoreCreateBinary();
    s->gone = xSemaphoreCreateBinary();
    if (!s->done || !s->jobs || !s->slots || !s->progress || !s->gone) {
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

    /* above the PHP task, so a band is queued as soon as the DMA before it ends */
    if (xTaskCreatePinnedToCore(st_flush_task, "st77916", TASK_STACK, s, uxTaskPriorityGet(NULL) + 1, &s->task,
                                TASK_CORE) != pdPASS) {
        s->task = NULL;
        st_release(s);
        return ESP_ERR_NO_MEM;
    }

    s->bl_on = true;
    st_backlight_set(s, true);
    return ESP_OK;
}

/* ops->push, the synchronous path (unused while begin is set, kept for the contract): send [x0, x1) x [y0, y1)
 * from px (rows in panel byte order) in bands, waiting for each DMA transfer. px goes out as is when the DMA can
 * read it, else through the first band. PHP task only, with no job in flight. */
static bm_err_t st_push(void *panel, int x0, int y0, int x1, int y1, const void *px)
{
    st_state *s = panel;
    if (s->failed) {
        return ESP_ERR_INVALID_STATE;
    }
    int w = x1 - x0;
    if (w <= 0 || y1 <= y0) {
        return BM_OK;
    }
    size_t row_bytes = (size_t) w * sizeof(uint16_t);
    bool direct = esp_ptr_dma_capable(px) && ((uintptr_t) px & 3u) == 0;
    int rows = direct ? y1 - y0 : (int) (s->band_px / (size_t) w);
    const uint8_t *src = px;
    if (!direct) {
        bm_err_t be = st_bands(s);
        if (be != BM_OK) {
            return be;
        }
    }

    xQueueReset(s->done);
    for (int y = y0; y < y1; y += rows) {
        int y_end = y + rows < y1 ? y + rows : y1;
        size_t n = (size_t) (y_end - y) * row_bytes;
        if (!direct) {
            memcpy(s->band[0], src, n);
        }
        esp_err_t e = esp_lcd_panel_draw_bitmap(s->panel, x0, y, x1, y_end, direct ? src : (const void *) s->band[0]);
        if (e != ESP_OK) {
            return e;
        }
        src += n;
        bm_err_t be = st_band_done(s);
        if (be != BM_OK) {
            return be;
        }
    }
    return BM_OK;
}

/* The band path (display_band_ops_t of ext-display 1.1). The renderer's task calls these; the flush task sends the
 * bands, in the order of push, through the same queue as the regions of the surface. */
#define BAND_ROWS 40
/* The ST77916 is a 360-wide part: a panel wider than that cannot take full-width 40-row bands. */
#define BAND_BYTES_MAX (360 * BAND_ROWS * sizeof(uint16_t))

static void *st_band_alloc(void *panel, size_t bytes)
{
    (void) panel;
    if (bytes == 0 || bytes > BAND_BYTES_MAX) {
        return NULL;
    }
    return heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

static void st_band_free(void *panel, void *px)
{
    (void) panel;
    heap_caps_free(px);
}

/* The window of every band is set by its esp_lcd_panel_draw_bitmap(); begin only checks and remembers the region. */
static bm_err_t st_band_begin(void *panel, int x0, int y0, int x1, int y1)
{
    st_state *s = panel;
    if (s->failed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 || x1 > s->width || y1 > s->height) {
        return BM_ERR_INVALID_ARG;
    }
    s->band_x0 = x0;
    s->band_y0 = y0;
    s->band_x1 = x1;
    s->band_y1 = y1;
    s->band_open = true;
    return BM_OK;
}

/* Takes a slot (blocking while BAND_SLOTS bands are in flight), queues the band for the flush task and returns. An
 * error leaves nothing queued and no slot held, so no done will run. */
static bm_err_t st_band_push(void *panel, int y0, int rows, const void *px, void (*done)(void *arg), void *arg)
{
    st_state *s = panel;
    if (s->failed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s->band_open || !px || rows < 1 || y0 < s->band_y0 || y0 + rows > s->band_y1 ||
        (size_t) rows * (size_t) (s->band_x1 - s->band_x0) * sizeof(uint16_t) > BAND_BYTES_MAX) {
        return BM_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s->slots, pdMS_TO_TICKS(BAND_ACCEPT_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    st_job_t j = {
        .kind = ST_JOB_BAND,
        .band = { .x0 = s->band_x0, .x1 = s->band_x1, .y0 = y0, .rows = rows, .px = px, .done = done, .arg = arg },
    };
    if (xQueueSend(s->jobs, &j, 0) != pdTRUE) {
        xSemaphoreGive(s->slots);
        return ESP_ERR_NO_MEM;
    }
    return BM_OK;
}

/* Every band was given to push: nothing to flush, and the done callbacks are not waited for. */
static bm_err_t st_band_end(void *panel)
{
    st_state *s = panel;
    s->band_open = false;
    return BM_OK;
}

/* The order in which the panel refreshes its picture, in the coordinates of the band path. The controller reads its
 * RAM row 0 first after each TE pulse (MADCTL ML = 0, as this driver's init sequences leave it); the orientation
 * maps our rows onto its RAM: mirror_y (MADCTL MY) turns them over, swap_xy (MV) makes them columns.
 * ST77916_SCAN_REVERSED=1 at build time reverses the result, for a panel module whose RAM is read the other way. */
#ifndef ST77916_SCAN_REVERSED
#define ST77916_SCAN_REVERSED 0
#endif

static display_scan_t st_scan(const st_state *s)
{
    bool rev = s->mirror_y != (ST77916_SCAN_REVERSED != 0);
    if (s->swap_xy) {
        return rev ? DISPLAY_SCAN_RIGHT_LEFT : DISPLAY_SCAN_LEFT_RIGHT;
    }
    return rev ? DISPLAY_SCAN_BOTTOM_UP : DISPLAY_SCAN_TOP_DOWN;
}

static const display_band_ops_t st_band_ops = {
    .rows = BAND_ROWS,
    .bytes_max = BAND_BYTES_MAX,
    .own_ram = true,   /* the panel keeps its picture */
    .buf_alloc = st_band_alloc,
    .buf_free = st_band_free,
    .begin = st_band_begin,
    .push = st_band_push,
    .end = st_band_end,
};

/* Waits until the bands already accepted are sent and their done ran, and keeps every slot: a push from the renderer
 * meanwhile blocks, and goes after the sleep or wake that called this. The caller gives the slots back with
 * st_band_resume(), on every path. On a timeout it holds none. */
static bm_err_t st_band_drain(st_state *s)
{
    for (int got = 0; got < BAND_SLOTS; got++) {
        if (xSemaphoreTake(s->slots, pdMS_TO_TICKS(BAND_ACCEPT_TIMEOUT_MS)) != pdTRUE) {
            while (got-- > 0) {
                xSemaphoreGive(s->slots);
            }
            return ESP_ERR_TIMEOUT;
        }
    }
    return BM_OK;
}

static void st_band_resume(st_state *s)
{
    for (int i = 0; i < BAND_SLOTS; i++) {
        xSemaphoreGive(s->slots);
    }
}

/* On/off only: there is no PWM on the backlight pin. */
static bm_err_t st_backlight(void *panel, int level)
{
    st_state *s = panel;
    s->bl_on = level > 0;
    st_backlight_set(s, s->bl_on);
    return BM_OK;
}

/* sleep: display off, backlight off. wake: re-initialise the panel (a light sleep may have disturbed the QSPI
 * lines), backlight back on, and have the next flush() send the full frame. */
static bm_err_t st_sleep_locked(st_state *s, bool on)
{
    display_quiesce(s);   /* another surface on this panel may still be sending: esp_lcd's IO is not shared */
    if (on) {
        esp_err_t e = esp_lcd_panel_disp_on_off(s->panel, false);
        if (e != ESP_OK) {
            return e;
        }
        st_backlight_set(s, false);
        return BM_OK;
    }
    st_reset_pulse(s);
    esp_err_t e = esp_lcd_panel_init(s->panel);
    if (e == ESP_OK) {
        e = st_configure(s);
    }
    if (e != ESP_OK) {
        return e;
    }
    s->bl_ready = false;
    st_backlight_set(s, s->bl_on);
    display_invalidate(s);
    return BM_OK;
}

static bm_err_t st_sleep(void *panel, bool on)
{
    st_state *s = panel;
    bm_err_t e = st_band_drain(s);   /* the bands of a renderer first; its pushes wait until we are done */
    if (e != BM_OK) {
        return e;
    }
    e = st_sleep_locked(s, on);
    st_band_resume(s);
    return e;
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
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, te, IS_LONG, 0, "-1")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, buffered, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, vsync, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_st_fill, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_st_onboard, 0, 0, Baremetal\\Spi\\Driver\\St77916, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, block, IS_STRING, 0, "\"display\"")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, buffered, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, vsync, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

PHP_METHOD(St77916, __construct)
{
    zval *zbus;
    zend_long cs = 21, rst = 3, bl = -1, width = 360, height = 360, x_gap = 0, y_gap = 0, te = -1;
    bool mirror_x = 0, mirror_y = 0, swap_xy = 0, invert = 0, buffered = 0, vsync = 0;
    ZEND_PARSE_PARAMETERS_START(1, 15)
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
        Z_PARAM_LONG(te)
        Z_PARAM_BOOL(buffered)
        Z_PARAM_BOOL(vsync)
    ZEND_PARSE_PARAMETERS_END();

    if (!spi_check_device_args(2, cs, 0, 0, 0, 0)) {   /* the same CS range as ext-spi's constructors */
        RETURN_THROWS();
    }
    if (rst < -1 || rst > 255 || bl < -1 || bl > 255) {
        zend_argument_value_error((rst < -1 || rst > 255) ? 3 : 4, "must be a GPIO number or -1");
        RETURN_THROWS();
    }
    if (te < -1 || te > 255) {
        zend_argument_value_error(13, "must be a GPIO number or -1");
        RETURN_THROWS();
    }
    if (vsync && te < 0) {
        zend_value_error("Baremetal\\Spi\\Driver\\St77916: vsync needs the TE pin (te:)");
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
            s->mirror_y != mirror_y || s->swap_xy != swap_xy || s->invert != invert || s->te != (int) te ||
            s->buffered != buffered || s->vsync != vsync) {
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
        s->te = (int) te;
        s->buffered = buffered;
        s->vsync = vsync;
        s->band_ops = st_band_ops;
        s->band_ops.scan = st_scan(s);
        s->ops = (display_panel_ops_t){
            .width = (uint16_t) width, .height = (uint16_t) height, .format = DISPLAY_FORMAT_RGB565,
            .refresh_hz = ST_REFRESH_HZ,
            .push = st_push, .backlight = bl >= 0 ? st_backlight : NULL, .sleep = st_sleep,
            .begin = st_begin, .wait = st_wait,
            .band = &s->band_ops,
        };
        esp_err_t e = st_hw_init(s, bo->bus->host, (int) cs);
        if (e != ESP_OK) {
            zend_throw_exception_ex(zend_ce_exception, 0, "st77916 init failed: %s", esp_err_to_name(e));
            RETURN_THROWS();
        }
        s->inited = true;
    } else {
        /* another surface on the same panel: let the flushes of the others end first */
        display_quiesce(s);
    }
    display_attach_opts_t opts = { .buffered = buffered, .vsync = vsync, .te = (int) te };
    bm_err_t ae = display_attach_ex(Z_OBJ_P(ZEND_THIS), &s->ops, s, &opts);
    if (ae == BM_ERR_INVALID_ARG) {
        zend_value_error("Baremetal\\Spi\\Driver\\St77916: vsync needs the TE pin (te:)");
        RETURN_THROWS();
    }
    if (ae != BM_OK) {
        zend_throw_exception_ex(zend_ce_exception, 0, "st77916: cannot attach the drawing surface (error 0x%x)",
                                (unsigned) ae);
        RETURN_THROWS();
    }
}

/* fill(int color): clear() + flush(). */
PHP_METHOD(St77916, fill)
{
    zval color;
    zend_long c;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(c)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    ZVAL_LONG(&color, c);
    zend_call_method_with_1_params(obj, obj->ce, NULL, "clear", NULL, &color);
    if (!EG(exception)) {
        zend_call_method_with_0_params(obj, obj->ce, NULL, "flush", NULL);
    }
}

static bool board_int(const char *id, const char *block, const char *key, bool required, int *out)
{
    int v;
    if (bm_board_int(block, key, &v)) {
        *out = v;
        return true;
    }
    if (required) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "board '%s': %s.%s is missing", id, block, key);
        return false;
    }
    *out = 0;
    return true;
}

/* onboard(string $block = 'display', bool $buffered = false, bool $vsync = false): the panel described by a block
 * of the board, on a QSPI Bus opened from it; the block's optional te key is the TE pin. */
PHP_METHOD(St77916, onboard)
{
    zend_string *zblock = NULL;
    bool buffered = 0, vsync = 0;
    ZEND_PARSE_PARAMETERS_START(0, 3)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(zblock)
        Z_PARAM_BOOL(buffered)
        Z_PARAM_BOOL(vsync)
    ZEND_PARSE_PARAMETERS_END();
    const char *block = zblock ? ZSTR_VAL(zblock) : "display";
    const char *id = bm_board_id();
    const char *type = bm_board_type(block);
    if (!type) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "board '%s' has no '%s' block", id, block);
        RETURN_THROWS();
    }
    if (strcmp(type, "st77916") != 0) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "board '%s' has a %s of type '%s', not 'st77916'", id,
                                block, type);
        RETURN_THROWS();
    }

    static const char *const bus_keys[] = { "sclk", "d0", "d1", "d2", "d3" };
    static const char *const panel_keys[] = { "cs", "rst", "bl", "width", "height" };
    static const char *const opt_keys[] = { "gap-x", "gap-y", "mirror-x", "mirror-y", "swap-xy" };
    int bus_v[5], panel_v[5], opt_v[5];
    for (int i = 0; i < 5; i++) {
        if (!board_int(id, block, bus_keys[i], true, &bus_v[i])) {
            RETURN_THROWS();
        }
    }
    for (int i = 0; i < 5; i++) {
        if (!board_int(id, block, panel_keys[i], true, &panel_v[i])) {
            RETURN_THROWS();
        }
    }
    for (int i = 0; i < 5; i++) {
        board_int(id, block, opt_keys[i], false, &opt_v[i]);
    }
    int te;
    if (!bm_board_int(block, "te", &te)) {
        te = -1;   /* optional: a panel without a TE pin */
    }
    if (vsync && te < 0) {
        zend_value_error("Baremetal\\Spi\\Driver\\St77916: vsync needs the TE pin (te:)");
        RETURN_THROWS();
    }

    zval bus, args[15];
    object_init_ex(&bus, spi_bus_ce);
    for (int i = 0; i < 5; i++) {
        ZVAL_LONG(&args[i], bus_v[i]);
    }
    zend_call_known_instance_method(spi_bus_ce->constructor, Z_OBJ(bus), NULL, 5, args);
    if (EG(exception)) {
        zval_ptr_dtor(&bus);
        RETURN_THROWS();
    }

    zend_class_entry *ce = EX(func)->common.scope;
    object_init_ex(return_value, ce);
    ZVAL_COPY_VALUE(&args[0], &bus);
    for (int i = 0; i < 5; i++) {
        ZVAL_LONG(&args[1 + i], panel_v[i]);
    }
    ZVAL_LONG(&args[6], opt_v[0]);
    ZVAL_LONG(&args[7], opt_v[1]);
    ZVAL_BOOL(&args[8], opt_v[2] != 0);
    ZVAL_BOOL(&args[9], opt_v[3] != 0);
    ZVAL_BOOL(&args[10], opt_v[4] != 0);
    ZVAL_FALSE(&args[11]);   /* invert */
    ZVAL_LONG(&args[12], te);
    ZVAL_BOOL(&args[13], buffered);
    ZVAL_BOOL(&args[14], vsync);
    zend_call_known_instance_method(ce->constructor, Z_OBJ_P(return_value), NULL, 15, args);
    zval_ptr_dtor(&bus);
    if (EG(exception)) {
        zval_ptr_dtor(return_value);
        ZVAL_NULL(return_value);
        RETURN_THROWS();
    }
}

/* Registers St77916::REFRESH_HZ (ext-spi calls this once the class exists). */
static void st_class_init(zend_class_entry *ce)
{
    zend_declare_class_constant_double(ce, "REFRESH_HZ", sizeof("REFRESH_HZ") - 1, (double) ST_REFRESH_HZ);
}

static const zend_function_entry st77916_methods[] = {
    PHP_ME(St77916, __construct, arginfo_st_ctor,    ZEND_ACC_PUBLIC)
    PHP_ME(St77916, fill,        arginfo_st_fill,    ZEND_ACC_PUBLIC)
    PHP_ME(St77916, onboard,     arginfo_st_onboard, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    DISPLAY_FE
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
    .class_init = st_class_init,
};
