---
eyebrow: 'esp32-display-st77916 · Recipe'
lede:    'A live bar chart with fillRect() and a region flush: send just the band that changes, never the whole panel.'
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
use Baremetal\Spi\Driver\St77916;

const BARS = 8;
const BAR_W = 24;
const GAP = 8;
const LEFT = 56;        // 8 bars of 24 px with 8 px gaps, centred on 360 px
const BASE = 280;       // bottom of the bars
const MAX_H = 200;

$lcd = St77916::onboard();

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
            $lcd->fillRect($x, BASE - $new, BAR_W, $new - $old, $bar);   // grow
        } elseif ($new < $old) {
            $lcd->fillRect($x, BASE - $old, BAR_W, $old - $new, $bg);    // shrink
        }
        $heights[$i] = $new;
    }
    $lcd->flush();   // only the rectangle around the changed strips
    delay(200);
}
```
<!-- @endcode-block -->

- Only the strips between the old and the new top of each bar are drawn, and `flush()` sends the rectangle that
  covers them, so a frame costs one region instead of a 360 x 360 push.
- Off-screen parts of a rectangle are clipped; a rectangle with a zero or negative size draws nothing.
- `rect()` would draw only the outline; the bars use `fillRect()`.
- On the round panel the corners of the 360 x 360 area are not visible; keep content inside the circle.
