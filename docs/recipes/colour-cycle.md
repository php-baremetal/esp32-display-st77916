---
eyebrow: 'esp32-display-st77916 · Recipe'
lede:    'The first thing to run on a new panel: bring it up and cycle full-screen colours once a second.'
see_also:
  - { href: '../usage/st77916.md', meta: 'PHP API' }
  - { href: '../reference/errors-and-limits.md', meta: 'Reference' }
prev: { label: 'Errors, limits and light sleep', href: '../reference/errors-and-limits.md' }
next: { label: 'Draw bars', href: './draw-bars.md' }
---

# Cycle colours

The ST77916 panel of a Waveshare ESP32-S3-Touch-LCD-1.85B: QSPI on SCLK 40 and data lines 46, 45, 42, 41, CS 21,
reset 3, backlight 5. On another board, change the board id and the pins.

<!-- @code-block language="json" label="composer.json" -->
```json
{
  "name": "acme/display-hello",
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
      "sdk": "esp32@^2.0",
      "board": "waveshare-esp32-s3-touch-lcd-1.85b",
      "runtime": "init-loop",
      "storage": "embedded"
    }
  }
}
```
<!-- @endcode-block -->

<!-- @code-block language="php" label="project-src/index.php" -->
```php
<?php
use Baremetal\Spi\Bus;
use Baremetal\Spi\Driver\St77916;

$bus = new Bus(sclk: 40, mosi: 46, miso: 45, data2: 42, data3: 41);   // mosi = D0, miso = D1
$lcd = new St77916($bus, cs: 21, rst: 3, bl: 5, width: 360, height: 360);

function loop(int $tick): void
{
    global $lcd;

    $colors = [$lcd->rgb(255, 0, 0), $lcd->rgb(0, 255, 0), $lcd->rgb(0, 0, 255), $lcd->rgb(255, 255, 255)];
    $lcd->fill($colors[$tick % 4]);
    delay(1000);
}
```
<!-- @endcode-block -->

The panel turns red, green, blue and white, one second each, for as long as the board runs.

## When it fails

- **Black panel, no exception**: the backlight pin is wrong or `bl` was left at `-1`.
- **Wrong colours or a shifted picture**: try `invert: true`, or set `x_gap` / `y_gap` for a panel whose
  visible area does not start at the controller's origin.
- **`cannot open SPI host 1: ...`**: the host is taken; see ext-spi's Bus page on its
  [package page](package:php-baremetal/ext-spi).
- **`st77916 init failed: ESP_ERR_NO_MEM`**: not enough DMA-capable internal RAM for the band buffer
  (`width` x 80 bytes).
