---
eyebrow: 'esp32-display-st77916 · Recipe'
lede:    'A live bar chart with rect() only: repaint just the bars that change, never the whole panel.'
see_also:
  - { href: '../usage/st77916.md', meta: 'PHP API' }
  - { href: './colour-cycle.md', meta: 'Recipe' }
prev: { label: 'Cycle colours', href: './colour-cycle.md' }
next: { label: 'No next page', href: '#' }
---

# Draw bars

Eight bars that move every 200 ms on the Waveshare ESP32-S3-Touch-LCD-1.85B. Use the project `composer.json`
from [Cycle colours](./colour-cycle.md). The values here are random; feed in a sensor reading instead.

<!-- @code-block language="php" label="project-src/index.php" -->
```php
<?php
use Baremetal\Spi\Bus;
use Baremetal\Spi\Driver\St77916;

const BARS = 8;
const BAR_W = 24;
const GAP = 8;
const LEFT = 56;        // 8 bars of 24 px with 8 px gaps, centred on 360 px
const BASE = 280;       // bottom of the bars
const MAX_H = 200;

$lcd = new St77916(new Bus(sclk: 40, mosi: 46, miso: 45, data2: 42, data3: 41), cs: 21, rst: 3, bl: 5);

$bg  = $lcd->rgb(0, 0, 0);
$bar = $lcd->rgb(0, 200, 255);
$lcd->fill($bg);

$heights = array_fill(0, BARS, 0);

function loop(int $tick): void
{
    global $lcd, $bg, $bar, $heights;

    for ($i = 0; $i < BARS; $i++) {
        $x = LEFT + $i * (BAR_W + GAP);
        $old = $heights[$i];
        $new = random_int(10, MAX_H);

        if ($new > $old) {
            $lcd->rect($x, BASE - $new, BAR_W, $new - $old, $bar);   // grow
        } elseif ($new < $old) {
            $lcd->rect($x, BASE - $old, BAR_W, $old - $new, $bg);    // shrink
        }
        $heights[$i] = $new;
    }
    delay(200);
}
```
<!-- @endcode-block -->

- Only the strip between the old and the new top of each bar is painted, so a frame costs a few thin
  rectangles instead of a 360 x 360 fill.
- Off-panel parts of a rectangle are clipped; a rectangle with a negative size paints nothing.
- On the round panel the corners of the 360 x 360 area are not visible; keep content inside the circle.
