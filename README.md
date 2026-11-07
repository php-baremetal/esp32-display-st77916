# esp32-display-st77916

A driver for ST77916 QSPI round displays (360 x 360 by default), for [ext-spi](package:php-baremetal/ext-spi) and
[ext-display](package:php-baremetal/ext-display). It adds `Baremetal\Spi\Driver\St77916`, a `Baremetal\Spi\Device`
that implements `Baremetal\Output\Display`: the panel gets a framebuffer and every drawing method of ext-display
(primitives, text, images, region flush), plus backlight and sleep. `St77916::REFRESH_HZ` (60 Hz, also
`refreshRate()`) is the panel's nominal refresh rate; measured on a Waveshare ESP32-S3-Touch-LCD-1.85B, the
panel runs at 60.03 to 60.08 Hz. It runs on ESP32 only (it uses ESP-IDF's
`esp_lcd`).

## Install

In a project created with `phpbm init`:

```bash
composer require php-baremetal/esp32-display-st77916
```

The driver requires ext-spi and ext-display, so this line is enough.

It declares the board block `display:st77916`, so `phpbm init` and `phpbm board` offer it on boards with an on-board ST77916 panel.

## Example

On a board whose description has the panel (the Waveshare ESP32-S3-Touch-LCD-1.85B), `onboard()` opens it with
the board's pins:

```php
<?php
use Baremetal\Spi\Driver\St77916;

$lcd = St77916::onboard();

$lcd->clear($lcd->rgb(0, 0, 64));                            // dark blue
$lcd->fillRect(130, 130, 100, 100, $lcd->rgb(255, 160, 0));  // orange square in the middle
$lcd->text(120, 250, 'Hello', $lcd->rgb(255, 255, 255));
$lcd->flush();                                               // drawing goes to the panel here
```

On another board, pass the bus and pins: `new St77916(new Bus(sclk: 40, mosi: 46, miso: 45, data2: 42,
data3: 41), cs: 21, rst: 3, bl: 5)`.

## Asynchronous flush

`flush()` returns at once: a FreeRTOS flush task of the driver, on core 1 and one priority above the PHP task,
sends the region through two ping-pong bands of 20 rows (in DMA-capable internal RAM, 14 400 bytes each at 360
wide): while one band is in DMA, the task gathers the next. A drawing waits only when it touches rows not sent
yet; `flushWait()` waits for the whole flush.

Three constructor options go with it, after `$invert`:

| Option | Default | Effect |
| --- | --- | --- |
| `te` | `-1` | The GPIO of the panel's TE (tearing effect) output. Needed by `vsync` and `onVsync()`. |
| `buffered` | `false` | A second framebuffer (`width` x `height` x 2 bytes, 259 200 bytes at 360 x 360, PSRAM on boards that have it): `flush()` copies the region there and the panel reads the copy, so drawing never waits for a flush. |
| `vsync` | `false` | Every region starts at the next TE pulse. Needs `te`: without it, `\ValueError` `Baremetal\Spi\Driver\St77916: vsync needs the TE pin (te:)`. |

`onboard(string $block = 'display', bool $buffered = false, bool $vsync = false)` takes the TE pin from the
block's optional `te` key (GPIO 18 on the Waveshare ESP32-S3-Touch-LCD-1.85B):

```php
$lcd = St77916::onboard(vsync: true);   // each flush() starts at the panel's next frame
```

The TE pin belongs to the display: ext-display attaches its interrupt there, so a `watch_gpio()` on the same pin
takes it away from the display.

## Documentation

- [Overview](docs/overview.md): requirements, install, a first project, how it registers.
- [St77916](docs/usage/st77916.md): `onboard()`, the constructor and the panel's methods.
- [Errors, limits and sleep](docs/reference/errors-and-limits.md)
- [Cycle colours](docs/recipes/colour-cycle.md) and [Draw bars](docs/recipes/draw-bars.md)

## License

MIT. `src/esp_lcd_st77916.c`, `src/esp_lcd_st77916.h` and the init tables in `src/st77916_init_cmds.h` come
from Espressif's ST77916 `esp_lcd` reference driver (Apache-2.0).
