# esp32-display-st77916

A driver for ST77916 QSPI round displays (360 x 360 by default), for [ext-spi](package:php-baremetal/ext-spi). It
adds `Baremetal\Spi\Driver\St77916`, a `Baremetal\Spi\Device` that implements `Baremetal\Output\Display`: bring
the panel up, fill it or paint solid rectangles in RGB565, and re-initialise it after a light sleep. It runs on
ESP32 only (it uses ESP-IDF's `esp_lcd`).

## Install

In a project created with `phpbm init`:

```bash
composer require php-baremetal/esp32-display-st77916
```

The driver requires ext-spi, so this line is enough.

## Example

The panel of a Waveshare ESP32-S3-Touch-LCD-1.85B:

```php
<?php
use Baremetal\Spi\Bus;
use Baremetal\Spi\Driver\St77916;

$bus = new Bus(sclk: 40, mosi: 46, miso: 45, data2: 42, data3: 41);   // QSPI
$lcd = new St77916($bus, cs: 21, rst: 3, bl: 5);

$lcd->fill($lcd->rgb(0, 0, 64));                         // dark blue
$lcd->rect(130, 130, 100, 100, $lcd->rgb(255, 160, 0));  // orange square in the middle
```

## Documentation

- [Overview](docs/overview.md): requirements, install, a first project, how it registers.
- [St77916](docs/usage/st77916.md): constructor and methods.
- [Errors, limits and light sleep](docs/reference/errors-and-limits.md)
- [Cycle colours](docs/recipes/colour-cycle.md) and [Draw bars](docs/recipes/draw-bars.md)

## License

MIT. `src/esp_lcd_st77916.c`, `src/esp_lcd_st77916.h` and the init tables in `src/st77916_init_cmds.h` come
from Espressif's ST77916 `esp_lcd` reference driver (Apache-2.0).
