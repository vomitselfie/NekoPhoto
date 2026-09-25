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
    double brushSize = 0;      // diameter in document pixels; 0 keeps the preset's own
    bool eraser = false;
    double hardness = -1;      // 0..1; negative keeps the current setting
    double reach = 0.35;
    double zoom = 0;
    int burst = 1;             // moves delivered between two repaints: a 1000 Hz mouse gives ~16 per 60 Hz frame           // view zoom (1 = 100%); 0 fits the document in the window       // the stroke's half-width as a fraction of the visible document's short side
};

/// Runs the benchmark in `window` (shown), prints the results to stdout, and returns an exit code.
int runBrushBench(MainWindow& window, const BrushBenchOptions& options);

struct ViewBenchOptions {
    QSize document{4096, 4096};
    int layers = 5;
};
int runViewBench(MainWindow& window, const ViewBenchOptions& options);

} // namespace app
