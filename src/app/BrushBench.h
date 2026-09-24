// A developer benchmark for brush latency: `nekophoto --bench-brush <preset>` paints strokes through the real
// canvas with synthetic mouse events and reports how long a press takes to show its first paint, how long
// each move takes to show, and how long a release takes. The time includes the event handler (the brush) and
// the repaint that follows (the canvas), but not the display server's frame pacing.
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

} // namespace app
