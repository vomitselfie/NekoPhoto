// The editor's tools and their kinds (EditorSession::tool, the tool rail, the options bar), and why a tool can refuse
// the active layer. Small on purpose: a file that only names a tool needs nothing else of the session.
#pragma once
#include <QString>

namespace app {

enum class Tool { Move, Marquee, Lasso, Wand, Scribble, Crop, Brush, SpotHealing, CloneStamp, Smudge, Gradient, Shape, Eyedropper, Hand, Zoom, Text, Dodge, PaintBucket, Pen, DirectSelect, Artboard, Slice };
enum class MarqueeKind { Rectangle, Ellipse };

/// Why a tool cannot work on the active layer (Photoshop's forbidden cursor), and what a press does about it: says
/// why, offers the smart object's Edit Contents or Rasterize, or offers to rasterize a type, shape or fill layer.
struct ToolRefusal {
    enum Kind { Refused, SmartObject, Rasterize };
    Kind kind = Refused;
    QString reason;
};

/// What rasterizing would take from a layer: its type, its shape (a solid, gradient or pattern fill cut by a path), or
/// its fill (a solid colour, gradient or pattern fill layer).
enum class Rasterizable { No, Type, Shape, Fill };
enum class LassoKind { Freehand, Polygonal };
enum class BlurToolMode { Liquify, Blur, Smudge, Sharpen };

} // namespace app
