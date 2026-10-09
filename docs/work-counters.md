# Work counters

Timing tests are flaky on a shared machine and on CI, so regressions in *how much work* an operation does are caught
by counting it instead. `compositor/workcounters.h` keeps a handful of counters that the core bumps once per call
(never per pixel) with the area that call covered; two ctest suites run fixed, scripted workloads and assert upper
bounds on them. The benchmarks in [benchmarks.md](benchmarks.md) and [brush-latency.md](brush-latency.md) still say
how *fast* the work is; these tests say how much of it there is, and they run in every ctest.

## The counters

| Counter | Bumped by | Amount |
|---|---|---|
| `render.calls`, `render.pixels` | `render()`, `render16()`, `renderF()`, `renderNative()` (CMYK and Lab) | one call, its output pixels |
| `layer.draws`, `layer.drawPixels` | `drawLayer` at every depth and mode: one layer resampled and composited | one draw, the output pixels its footprint covered |
| `adjustment.passes`, `adjustment.pixels` | an adjustment layer applied while rendering (8-bit and the deep executors) | one per layer (each of a fused run of table adjustments), the frame pixels passed over (a fused run once) |
| `mip.hits`, `mip.misses`, `mip.builtPixels` | `MipCache::level` asked for a reduction | a hit when it held it; a miss and the built levels' pixels when it had to build |
| `mip.refreshPixels` | `MipCache::refresh`, after a stroke wrote its working pixels in place | the level pixels brought up to date |
| `brush.dabs`, `brush.recomposePixels` | `BrushStroke::dab` (the round brush), `BrushStroke::recompose` | one dab; the working pixels recomposed |
| `smartFilter.stacks`, `smartFilter.passes` | `renderSmartFilterStack` (8 and 16 bits) | one stack drawn; each enabled entry run |

They are relaxed atomics (`work::add`), bumped a few times per layer drawn, so they stay compiled in; the cost is
below measurement. Counts are sums of rectangles fixed by the call, not by how `parallelFor` split the rows, so they
are the same at any worker count (the core tests give the same counts with one worker and with 24) and on any platform.
`work::snapshot()` takes all of them at once; the difference of two snapshots is the work done in between, and
`Snapshot::describe()` lists the nonzero ones for a failure message.

## The tests

`work_counter_tests` (tests/work_counter_tests.cpp), the core alone:

- a 40 px brush press and twenty 12 px moves on a 4000 × 3000 document, each step rendered as the canvas renders
  it (the dirty rectangle, the stroke as an override, a `RenderCache`): dabs, recomposed pixels and rendered pixels
  per step and for the stroke, no reduction built at 1:1;
- the same stroke seen at 25 %: the layer's reductions are refreshed over the dirty area only, never rebuilt;
- thirty ticks of a Levels layer's white input over five 2400 × 1600 layers at 25 %: each tick draws each pixel layer
  once, makes one adjustment pass, renders one frame, and builds no reduction;
- a second frame at the same zoom builds no reduction;
- a short stroke and a 16 × 16 edit on a 4000 × 3000 layer: the history keeps their regions' crops (and a
  thumbnail), not the 48 MB layer;
- adding two Smart Filters runs the stack once each; rendering (twice, zoomed out, after an unrelated change) runs
  none;
- consistency, not counters: five regions rendered alone equal the same regions cut from the full render, at 8 and
  16 bits, exactly (a rotated layer between pixels, a masked Multiply layer, a Curves layer); a render at 50 % and
  25 % (drawn from the reductions) is within 64 levels at worst and 0.5 on average of the full render halved.

`work_counters_selftest` (`nekophoto --self-test work-counters`, src/app/SelfTestWork.cpp), the whole window at a 1×
display scale, on a 4000 × 3000 document of five layers:

- a brush press and twenty moves at 100 % through `EditorSession` and the canvas's repaint: rendered pixels per
  press and per move (the dab's box and the canvas's 2-pixel margin), the release renders nothing
  (`documentChangedAsShown`), the stroke's history bytes, undo and redo render only the stroke's region;
- twenty ticks of a Levels layer's slider at fit zoom: one render of the view a tick (no panel renders the document
  again), each layer drawn once, one adjustment pass, no reduction built (renders and passes allow a quarter more than
  the ticks, for repaints the window system asks for on its own on CI machines);
- zoom to 40 %, ten 16-pixel pans down and ten right, zoom to 20 % and back: one render per zoom, a pan renders exactly
  the strip that came into view, no reduction built by a pan or by returning to a zoom.

The canvas's size depends on the platform's fonts and docks, so view-sized bounds are taken from the canvas's own
size; dab-sized bounds are absolute.

## Reading a failure, updating a bound

A failing bound prints the step, the counter, the bound and the measured value, then every nonzero counter of that
step:

```
FAIL slider ticks: layer.draws = 120, expected at most 100 (all: render.calls=20 render.pixels=6067440 ...)
```

Each bound sits next to a comment with the value measured when it was set and how the bound follows from it: about
1.3× the measured value where the count depends on a stroke's shape, and the measured value itself where it follows
from the geometry alone (layers × ticks, the strip a pan reveals). Both suites print every measured value on a
passing run (`build/tests/work_counter_tests`, or `ctest -R work_counters_selftest -V`), so after a change that is
meant to alter the work:

1. run the suite and read the new values;
2. if the work went down, lower the bound to the new value with the same headroom, and update the comment's number;
3. if it went up on purpose, say why in the commit message, raise the bound the same way, and update the comment.

Never raise a bound to make an unexplained failure pass: a counter that moved is the thing to explain.

## Found

What the counters showed when they were added (NekoPhoto 1.8.10):

- **A layer made inside the history keeps its first raster in the history once it is painted on.** The self-test's
  stroke on a 4000 × 3000 layer kept 206 KB when the document came from a file, but 48.2 MB when the layer had been
  imported by a step still in the history: the stroke's step keeps only crops (the region patch), and the steps
  before it that hold the same buffer unchanged hand it to the chain (`inheritPatched`), but the step that created the
  layer has no "before" for that slot, so its "after" keeps the whole first raster. Fixing it means a slot inherited on
  one side only, through `materialize`, undo, redo, `squash` and `trim`: not a small change, so it is left for its own
  piece of work. The self-test starts its history after building the document (`markOpened`), as opening a file
  does, and bounds that case.
- **An adjustment slider tick redraws every layer below the adjustment layer.** A `RenderCache` keeps the composite
  below a pixel layer being painted, but nothing is kept around an adjustment layer being dragged, so each tick
  composites every layer again (five draws over the view for one changed table). The mip cache makes each draw cheap,
  and the counters bound it at one draw per layer per tick; caching the backdrop below the dragged adjustment would
  make a tick one adjustment pass plus the layers above. A design change in `RenderPlan::editedIndex` and
  `runCached`, not made here.
- Nothing else: the dab, move, release, undo and redo renders follow the dirty rectangles; pans render only the
  strip; no panel renders the document on a slider tick; reductions are built once per level and refreshed, not
  rebuilt, under a stroke; Smart Filters run only when their stack or contents change.
