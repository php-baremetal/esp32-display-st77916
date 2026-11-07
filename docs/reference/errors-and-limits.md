---
eyebrow: 'esp32-display-st77916 · Reference'
lede:    'Every exception St77916 throws, the fixed sizes and timeouts behind them, and how sleep() and wake() bring the panel back after a light sleep.'
see_also:
  - { href: '../usage/st77916.md', meta: 'PHP API' }
  - { href: 'package:php-baremetal/ext-display#docs/reference/errors-and-limits.md', meta: 'Extension', label: 'ext-display errors and limits' }
  - { href: 'package:php-baremetal/ext-spi#docs/reference/errors-and-limits.md', meta: 'Extension', label: 'ext-spi errors and limits' }
  - { href: 'package:php-baremetal/ext-sys', meta: 'Extension', label: 'ext-sys (sys_light_sleep)' }
prev: { label: 'St77916', href: '../usage/st77916.md' }
next: { label: 'Cycle colours', href: '../recipes/colour-cycle.md' }
---

# Errors, limits and sleep

St77916 throws `\RuntimeException` from `onboard()` and from the drawing methods, plain `\Exception` from the
constructor, and `\ValueError` for an invalid argument. Constructor messages end with the ESP-IDF error name
(`esp_err_to_name()`); the drawing methods' messages, which come from ext-display, end with the error code.

## Exceptions

| Message | Thrown by | Cause |
| --- | --- | --- |
| `board '<id>' has no '<block>' block` | `onboard()` | The board the firmware was built for has no such block (`\RuntimeException`). |
| `board '<id>' has a <block> of type '<type>', not 'st77916'` | `onboard()` | The block describes another panel (`\RuntimeException`). |
| `board '<id>': <block>.<key> is missing` | `onboard()` | A required key (`sclk`, `d0` to `d3`, `cs`, `rst`, `bl`, `width`, `height`) is missing or not a number (`\RuntimeException`). |
| `cannot reserve st77916 (cs=N): ...` | constructor | The CS is held by another device or driver on the bus, or all 8 ext-spi device slots are in use. |
| `display on cs N already initialised with different parameters` | constructor | The panel on this bus and CS is already running with other pins, size or orientation. |
| `st77916 init failed: ...` | constructor | No internal RAM for the two bands or no memory for the flush task (`ESP_ERR_NO_MEM`), or `esp_lcd` could not open the panel IO or initialise the panel. |
| `st77916: cannot attach the drawing surface (error 0x101)` | constructor | No memory for the `width` x `height` x 2 bytes framebuffer, or for its copy with `$buffered` (`BM_ERR_NO_MEM`; another code if the surface could not be recorded). |
| `Spi\Bus is not initialised` | constructor | The `Bus` object was created without its constructor running. |
| `St77916: push failed (error 0x107)` | the call after a failed flush | A band did not finish within 1 second (`ESP_ERR_TIMEOUT`), or could not be queued (another code). |
| `St77916: push failed (error 0x103)` | the call after a failed flush | A timed-out transfer was still in flight after a further second, or the flush task could not take the region (`ESP_ERR_INVALID_STATE`); after a timed-out transfer the panel refuses all further flushes until the board restarts. |
| `St77916: flush timed out` | a drawing method, `flushWait()` | A wait for the flush in flight took more than 2000 ms. |
| `St77916: sleep failed (error 0x..)` | `sleep()` | The display-off command could not be sent. |
| `St77916: wake failed (error 0x..)` | `wake()` | The init sequence or the orientation could not be sent. |
| `St77916 is not initialised` | any `Output\Display` method | The constructor did not complete (`\Error`). |

`\ValueError` (`must be between 1 and 4096`) is thrown for a `$width` or `$height` out of range,
`\ValueError` (`must be a GPIO number or -1`) for a `$cs`, `$rst`, `$bl` or `$te` outside -1 to 255,
`\ValueError` (`Baremetal\Spi\Driver\St77916: vsync needs the TE pin (te:)`) for `$vsync` without a TE pin
(constructor, or `onboard()` on a block without `te`), and the drawing
methods throw the `\ValueError`s listed in ext-display's
[errors and limits](package:php-baremetal/ext-display#docs/reference/errors-and-limits.md).

## Limits

| Limit | Value |
| --- | --- |
| Panel size | 1 to 4096 pixels per side; 360 x 360 by default |
| Pixel format | RGB565, 16 bits per pixel, RGB element order |
| Refresh rate | `St77916::REFRESH_HZ` = 60 Hz, nominal for this driver's init sequences, not measured |
| Panel IO clock | 40 MHz, quad mode, SPI mode 0 (5 MHz for the ID read at init) |
| Framebuffer | `width` x `height` x 2 bytes per object (259 200 bytes at 360 x 360), from the system heap: PSRAM on boards that have it |
| Bands | two of `width` x 20 pixels (14 400 bytes each at 360 wide), DMA-capable internal RAM, kept for the whole run |
| Direct transfer | 60 rows, for a full-width region whose rows are in DMA-capable internal RAM |
| Flush task | 3072 bytes of stack, core 1, one priority above the task that constructed the panel; kept for the whole run |
| Copy with `$buffered` | `width` x `height` x 2 bytes (259 200 bytes at 360 x 360), from the system heap |
| Wait per band | 1000 ms, then 1000 ms more for the transfer to drain |
| Backlight | On or off: any `backlight()` level above 0 is on; no PWM |
| Panels | one per bus and CS; each takes one of the 8 ext-spi device slots |

## Sleep and light sleep

`sleep()` sends the display-off command and drives the backlight pin low. `wake()` re-initialises the panel, as
the constructor did: it pulses reset (when `$rst` is not `-1`), re-sends the init sequence, re-applies the
orientation, gaps and inversion, turns the display on and puts the backlight back as the last `backlight()` left
it (on by default). The panel's picture is lost, so `wake()` marks the whole framebuffer as changed: the next
`flush()` sends the full frame.

With the SDK option `power-save` on, or around an explicit `sys_light_sleep()` from ext-sys, a light sleep may
disturb the QSPI lines. Call `sleep()` before it and `wake()` after, then `flush()`.

<!-- @code-block language="php" label="sleep, then bring the panel back" -->
```php
<?php
use Baremetal\Spi\Driver\St77916;

$lcd = St77916::onboard();
$lcd->fill($lcd->rgb(0, 64, 0));

$lcd->sleep();
$cause = sys_light_sleep(5000);   // ext-sys: sleep 5 s, returns the wake cause
$lcd->wake();                     // re-initialises the panel
$lcd->flush();                    // sends the whole picture again
```
<!-- @endcode-block -->
