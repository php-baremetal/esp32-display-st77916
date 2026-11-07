# Changelog

## 1.1.0

- Added: ide/esp32-display-st77916.stub, the PHP declarations of the package for IDEs (conductor copies it into vendor/php-baremetal/ide-stubs/).
- Added: asynchronous flush: `flush()` returns at once, and a flush task on core 1 sends the region through two
  ping-pong bands of 20 rows in internal RAM (the same memory as the 40-row band before), gathering one band
  while the other is in DMA.
- Added: constructor options `te` (the TE pin), `buffered` (a copy of the framebuffer that the panel reads) and
  `vsync` (each region starts at the next TE pulse; `\ValueError` without `te`; `sleep()` and `wake()` drop a
  region still waiting for the pulse); `onboard()` takes `buffered` and `vsync`, and the TE pin from the
  block's optional `te` key.
- Added: St77916::REFRESH_HZ and refreshRate() (Output\Display): the panel's nominal refresh rate, 60 Hz.
- Changed: St77916 is a panel of ext-display: it gets every Output\Display method (framebuffer, primitives, text, region flush); rect() now draws an outline, fill() is clear() + flush().
- Changed: requires php-baremetal/ext-spi ^1.1: ext-spi 1.0.2 still registers Output\Display, which ext-display now owns.
- Added: St77916::onboard(), from the board's display block.
- Added: extra.baremetal.board-blocks: ["display:st77916"], so phpbm offers this package as a board feature.

## 1.0.2

- Fixed: `wake()` re-initialised the panel but left the backlight pin as the sleep left it. It now drives the
  backlight high again, as the constructor does.
- Fixed: `$cs`, `$rst` and `$bl` were not checked. A value outside -1 to 255 now throws `\ValueError`
  (`must be a GPIO number or -1`), as for the ext-spi 1.0.2 constructors.
- Docs: `$width` and `$height` are the drawing area, so with `$swap_xy` on a panel that is not square they are
  passed swapped.

## 1.0.1

- Changed: the author's contact email.
- Docs: projects use `"sdk": "esp32@^2.0"`.

## 1.0.0

Initial release.
