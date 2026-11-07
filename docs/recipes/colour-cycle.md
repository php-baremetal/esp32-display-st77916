---
eyebrow: 'esp32-display-st77916 · Recipe'
lede:    'The first thing to run on a new panel: bring it up and cycle full-screen colours once a second.'
see_also:
  - { href: '../usage/st77916.md', meta: 'PHP API' }
  - { href: '../reference/errors-and-limits.md', meta: 'Reference' }
prev: { label: 'Errors, limits and sleep', href: '../reference/errors-and-limits.md' }
next: { label: 'Draw bars', href: './draw-bars.md' }
---

# Cycle colours

The ST77916 panel of a Waveshare ESP32-S3-Touch-LCD-1.85B, opened with `onboard()` from the board's `display`
block. On a board without that block, use the constructor with your pins (see [St77916](../usage/st77916.md)).

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
use Baremetal\Spi\Driver\St77916;

$lcd = St77916::onboard();

function loop(int $tick): void
{
    global $lcd;

    $colors = [$lcd->rgb(255, 0, 0), $lcd->rgb(0, 255, 0), $lcd->rgb(0, 0, 255), $lcd->rgb(255, 255, 255)];
    $lcd->fill($colors[$tick % 4]);   // clear() + flush()
    delay(1000);
}
```
<!-- @endcode-block -->

The panel turns red, green, blue and white, one second each, for as long as the board runs.

## When it fails

- **`board '...' has no 'display' block`**: the project's board has no panel description; pass the bus and pins
  to the constructor instead.
- **Black panel, no exception**: the backlight pin is wrong, or `bl` is `-1`.
- **Wrong colours or a shifted picture**: try `invert: true` with the constructor, or set the gaps (`gap-x`,
  `gap-y` in the board, `x_gap`, `y_gap` in the constructor) for a panel whose visible area does not start at the
  controller's origin.
- **`cannot open SPI host 1: ...`**: the host is taken; see ext-spi's Bus page on its
  [package page](package:php-baremetal/ext-spi).
- **`st77916 init failed: ESP_ERR_NO_MEM`**: not enough DMA-capable internal RAM for the band buffer
  (`width` x 80 bytes).
