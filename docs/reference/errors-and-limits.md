---
eyebrow: 'esp32-display-st77916 · Reference'
lede:    'Every exception St77916 throws, the fixed sizes and timeouts behind them, and how to bring the panel back after light sleep.'
see_also:
  - { href: '../usage/st77916.md', meta: 'PHP API' }
  - { href: 'package:php-baremetal/ext-spi#docs/reference/errors-and-limits.md', meta: 'Extension', label: 'ext-spi errors and limits' }
  - { href: 'package:php-baremetal/ext-sys', meta: 'Extension', label: 'ext-sys (sys_light_sleep)' }
prev: { label: 'St77916', href: '../usage/st77916.md' }
next: { label: 'Cycle colours', href: '../recipes/colour-cycle.md' }
---

# Errors, limits and light sleep

St77916 throws plain `\Exception` for runtime failures and `\ValueError` for an invalid size. Messages end with
the ESP-IDF error name (`esp_err_to_name()`).

## Exceptions

| Message | Thrown by | Cause |
| --- | --- | --- |
| `cannot reserve st77916 (cs=N): ...` | constructor | The CS is held by another device or driver on the bus, or all 8 ext-spi device slots are in use. |
| `display on cs N already initialised with different parameters` | constructor | The panel on this bus and CS is already running with other pins, size or orientation. |
| `st77916 init failed: ...` | constructor | No internal RAM for the band buffer (`ESP_ERR_NO_MEM`), or `esp_lcd` could not open the panel IO or initialise the panel. |
| `st77916 is not initialised` | any method | The constructor did not complete the panel init. |
| `st77916 fill failed: ...`, `st77916 rect failed: ...` | `fill()`, `rect()` | A band could not be queued, or a band did not finish within 1 second (`ESP_ERR_TIMEOUT`). |
| `st77916 failed: a display transfer never completed` | any method, after a timeout | A timed-out transfer was still in flight after a further second; the object refuses all further drawing until the board restarts. |
| `st77916 wake failed: ...` | `wake()` | The init sequence or orientation could not be sent. |
| `Spi\Bus is not initialised` | constructor | The `Bus` object was created without its constructor running. |

`\ValueError` (`must be between 1 and 4096`) is thrown for a `$width` or `$height` out of range, and
`\ValueError` (`must be a GPIO number or -1`) for a `$cs`, `$rst` or `$bl` outside -1 to 255.

## Limits

| Limit | Value |
| --- | --- |
| Panel size | 1 to 4096 pixels per side; 360 x 360 by default |
| Pixel format | RGB565, 16 bits per pixel, RGB element order |
| Panel IO clock | 40 MHz, quad mode, SPI mode 0 (5 MHz for the ID read at init) |
| Band buffer | `width` x 40 pixels, DMA-capable internal RAM, kept for the whole run |
| Wait per band | 1000 ms |
| Full-panel fill | about 17 ms at 360 x 360 on the Waveshare ESP32-S3-Touch-LCD-1.85B |
| Panels | one per bus and CS; each takes one of the 8 ext-spi device slots |

## Light sleep and wake()

With the SDK option `power-save` on, or after an explicit `sys_light_sleep()` from ext-sys, the chip may enter
light sleep. If that disturbs the QSPI lines, the panel shows garbage or goes blank. `wake()` pulses reset,
re-sends the init sequence and re-applies the orientation; then repaint.

<!-- @code-block language="php" label="sleep, then bring the panel back" -->
```php
<?php
use Baremetal\Spi\Bus;
use Baremetal\Spi\Driver\St77916;

$lcd = new St77916(new Bus(sclk: 40, mosi: 46, miso: 45, data2: 42, data3: 41), cs: 21, rst: 3, bl: 5);
$lcd->fill($lcd->rgb(0, 64, 0));

$cause = sys_light_sleep(5000);       // ext-sys: sleep 5 s, returns the wake cause
$lcd->wake();
$lcd->fill($lcd->rgb(0, 64, 0));      // the picture is not restored by wake()
```
<!-- @endcode-block -->

The backlight pin is driven high only at construction; `wake()` does not touch it.
