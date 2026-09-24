// Developer benchmarks through the real canvas, with synthetic input events; each time covers the event handler
// and the repaint that follows, not the display server's frame pacing (docs/brush-latency.md).
//   --bench-brush <preset>: how long a press takes to show its first paint, each move, and a release.
//   --bench-view: zoom steps, pan steps, and undo/redo, on a large multi-layer document.
#pragma once
#include <QSize>
#include <QString>

namespace app {

class MainWindow;

struct BrushBenchOptions {
    QString preset;            // a brush preset id ("classic/dry_brush"), or "round" for the plain brush
    QSize document{2000, 2000};
    bool paintOnOpaque = false; // paint on the opaque image layer instead of a blank layer above it
    int strokes = 5;
    int moves = 80;
};

/// Runs the benchmark in `window` (shown), prints the results to stdout, and returns an exit code.
int runBrushBench(MainWindow& window, const BrushBenchOptions& options);

struct ViewBenchOptions {
    QSize document{4096, 4096};
    int layers = 5;
};
int runViewBench(MainWindow& window, const ViewBenchOptions& options);

} // namespace app
