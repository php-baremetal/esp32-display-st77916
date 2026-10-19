---
eyebrow: 'Package · Driver'
lede:    'Baremetal\Spi\Driver\St77916 drives an ST77916 QSPI round panel through ext-spi and ESP-IDF esp_lcd: fill it, paint solid rectangles, re-initialise it after light sleep. ESP32 only.'
see_also:
  - { href: './usage/st77916.md', meta: 'PHP API' }
  - { href: './recipes/colour-cycle.md', meta: 'Recipe' }
  - { href: 'package:php-baremetal/ext-spi#docs/overview.md', meta: 'Extension', label: 'ext-spi' }
  - { href: 'package:php-baremetal/esp32-spi-hal', meta: 'HAL', label: 'esp32-spi-hal' }
prev: { label: 'No previous page', href: '#' }
next: { label: 'St77916', href: './usage/st77916.md' }
---

# ST77916 display

esp32-display-st77916 gives PHP an ST77916 round LCD on a four-line (QSPI) SPI bus, such as the 360 x 360 panel
of the Waveshare ESP32-S3-Touch-LCD-1.85B. `Baremetal\Spi\Driver\St77916` extends `Baremetal\Spi\Device` and
implements `Baremetal\Output\Display`. It paints with solid RGB565 colours: the whole panel (`fill()`) or a
rectangle (`rect()`). There is no framebuffer, no text and no bitmap drawing.

## Requirements

| Requires | Why |
| --- | --- |
| `php-baremetal/ext-spi` | `Baremetal\Spi\Bus`, the `Device` base class and the `spi_drivers` registry this driver registers into. |
| `php-baremetal/esp32-spi-hal` | The ESP32 SPI HAL. The driver talks to the panel through `esp_lcd` on the host that ext-spi opened, so it is ESP32-only. |

It also needs the ESP-IDF components `esp_lcd` and `esp_driver_gpio` (`idf-requires`), which the SDK adds to
the build. Requiring this package is enough: it pulls in ext-spi and its HAL.

## Install

<!-- @steps -->
- **Install conductor once per machine**

  `composer global require php-baremetal/conductor`

- **Create the project**

  `phpbm init my-display`, picking the board `waveshare-esp32-s3-touch-lcd-1.85b`.

- **Require the driver**

  `composer require php-baremetal/esp32-display-st77916`

- **Build and flash**

  `phpbm flash`, then `phpbm monitor` for the console.
<!-- @endsteps -->

The resulting `composer.json`:

<!-- @code-block language="json" label="composer.json" -->
```json
{
  "name": "acme/my-display",
  "type": "project",
  "repositories": [
    { "type": "composer", "url": "https://repository.php-baremetal.com", "canonical": true }
  ],
  "require": {
    "php-baremetal/conductor": "^1.0",
    "php-baremetal/ext-sys": "^1.0",
    "php-baremetal/esp32-display-st77916": "^1.0"
  },
  "config": {
    "allow-plugins": { "php-baremetal/conductor": true },
    "platform": { "php": "8.5.11" }
  },
  "extra": {
    "baremetal": {
      "sdk": "esp32@^1.0",
      "board": "waveshare-esp32-s3-touch-lcd-1.85b",
      "runtime": "init-loop",
      "storage": "embedded"
    }
  }
}
```
<!-- @endcode-block -->

`ext-sys` provides `delay()` for the script below.

## A first script

<!-- @code-block language="php" label="project-src/index.php" -->
```php
<?php
use Baremetal\Output\Display;
use Baremetal\Spi\Bus;
use Baremetal\Spi\Driver\St77916;

// Waveshare ESP32-S3-Touch-LCD-1.85B: QSPI on SCLK 40, D0 46, D1 45, D2 42, D3 41
$bus = new Bus(sclk: 40, mosi: 46, miso: 45, data2: 42, data3: 41);
$lcd = new St77916($bus, cs: 21, rst: 3, bl: 5, width: 360, height: 360);

printf("display: %s\n", $lcd instanceof Display ? 'yes' : 'no');

function loop(int $tick): void
{
    global $lcd;

    $colors = [$lcd->rgb(255, 0, 0), $lcd->rgb(0, 255, 0), $lcd->rgb(0, 0, 255), $lcd->rgb(255, 255, 255)];
    $lcd->fill($colors[$tick % 4]);
    delay(1000);
}
```
<!-- @endcode-block -->

## PHP API at a glance

| Method | Returns | Meaning |
| --- | --- | --- |
| `new St77916(Bus $bus, int $cs = 21, int $rst = 3, int $bl = -1, ...)` | | Reserve the panel on its CS and bring it up (first construction only). |
| `fill(int $color)` | `void` | Paint the whole panel one RGB565 colour. |
| `rect(int $x, int $y, int $w, int $h, int $color)` | `void` | Paint a solid rectangle, clipped to the panel. |
| `rgb(int $r, int $g, int $b)` | `int` | Pack 8-bit R, G, B into RGB565. |
| `wake()` | `void` | Re-initialise the panel after a light sleep. |

Full signatures in [St77916](./usage/st77916.md).

## Registration

The package registers the descriptor `st77916_driver_desc` into ext-spi's `spi_drivers` registry
(`extra.baremetal.register`). At startup ext-spi turns it into the class `Baremetal\Spi\Driver\St77916`,
implementing `Baremetal\Output\Display`. The descriptor is managed-io: the driver drives the wire through
`esp_lcd`, so no raw SPI device is attached on the CS pin and the inherited `transfer()`, `write()` and `read()`
throw. To write another SPI driver, see ext-spi's driver documentation on its
[package page](package:php-baremetal/ext-spi).

## Notes

- The panel lives for the whole run. A second `new St77916()` on the same bus and CS with the same arguments
  returns the running panel without re-initialising it; with different arguments it throws. In the
  `web-server` runtime the panel survives between requests.
- Painting runs on the PHP task and returns when the DMA transfers are done: a full 360 x 360 fill takes about
  17 ms (about 59 fills per second) on the Waveshare ESP32-S3-Touch-LCD-1.85B.
- The driver keeps a DMA buffer of `width` x 40 pixels (28.8 KB at 360 px) in internal RAM for the whole run.
- The pin defaults (`cs` 21, `rst` 3) match the board the driver was written for; take the CS, reset,
  backlight and the four data lines from your board's schematic.
