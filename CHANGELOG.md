# Changelog

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
