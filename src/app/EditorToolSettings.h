// Settings the session keeps for its tools and commands: Content-Aware Fill's request, the Gradient tool's, and the
// Shape tool's kinds and the shape being dragged out.
#pragma once
#include "nekophoto/gradient_method.h"
#include "nekophoto/shape.h"
#include "nekophoto/vectorlayer.h"
#include <QPointF>
#include <QRectF>
#include <QString>
#include <memory>
#include <string>

namespace app {

enum class GradientStyle { ForegroundToBackground, ForegroundToTransparent };

/// Content-Aware Fill's settings (its dialog, or the automation method).
struct ContentFillRequest {
    /// Auto: the window the fill scans around each patch; All: a wider window (nekophoto/inpaint.h). There is no
    /// user-drawn sampling area (docs/legal-boundaries.md, "Content-Aware Fill").
    enum class Sampling { Auto, All } sampling = Sampling::Auto;
    bool newLayer = false;
};

struct GradientSettings {
    nekophoto::GradientShape shape = nekophoto::GradientShape::Linear;
    GradientStyle style = GradientStyle::ForegroundToTransparent;
    /// An imported gradient preset by name (PresetLibrary); empty for `style`.
    QString preset;
    bool reversed = false;
    double opacity = 1;
    /// Photoshop's Method menu: the space the colours blend in (Classic: as before).
    nekophoto::GradientMethod method = nekophoto::GradientMethod::Classic;
};

/// A shape being dragged out with the Shape tool, in whole document pixels.
struct ShapeDraft {
    nekophoto::ShapeKind kind;
    QPointF anchor;
    QRectF rect;
    double cornerRadius = 0;
    QPointF end;                           // where the drag is (a Line runs from the anchor to here)
};

/// The Shape tool's kinds (Photoshop's U group) and settings. New shapes are vector shape layers (vectorlayer.h)
/// filled with the foreground colour.
enum class VectorShapeKind { Rectangle, Ellipse, Polygon, Line, Custom };
struct ShapeToolSettings {
    VectorShapeKind kind = VectorShapeKind::Rectangle;
    double cornerRadius = 0;
    int sides = 5;
    double starInset = 0;                  // 0: a polygon; above: a star, its inner points this far in (0..0.99)
    double lineWeight = 4;
    std::string custom = "Heart";
    bool fill = true;
    nekophoto::VectorPaint fillPaint;     // Solid: the foreground colour
    nekophoto::VectorStroke stroke;       // enabled false: none
};

/// The composite rendered once for many samples over an unchanged document (the Eyedropper dragged:
/// EditorSession::compositeColorAt).
struct CompositeSnapshot { std::shared_ptr<const nekophoto::Image> flat; std::shared_ptr<const nekophoto::Image16> deep; };

} // namespace app
