// Scrubby sliders, as in Photoshop: the label beside a numeric field is a drag handle. Dragging it
// sideways changes the value (Shift finer, Alt or Ctrl coarser); a click without a drag, or a double-click,
// puts the caret in the field as before.
#pragma once
#include <QString>
#include <QtCore/qnamespace.h>
#include <functional>

class QAbstractSpinBox;
class QApplication;
class QLabel;

namespace app::scrub {

/// Watches every spin box the application shows and makes the label beside it (in the same row of a box,
/// grid or form layout, or the label whose buddy it is, a slider in between allowed) its drag handle.
void install(QApplication& app);

/// Makes `label` scrub `field` explicitly (for a label the layout search would not find).
void attach(QLabel* label, QAbstractSpinBox* field);

/// Brackets every drag so the edits it makes become one undo step. `begin` gets the field's label text and
/// says whether it opened a group; `end` closes it. Set by the main window.
void setUndoGroupHooks(std::function<bool(const QString&)> begin, std::function<void()> end);

/// The value a drag of `pixels` gives, for tests: `step` per pixel, times 0.1 with Shift, 10 with Alt or Ctrl,
/// clamped to [min, max] and rounded to `decimals`.
double scrubbedValue(double start, double pixels, double step, double min, double max, int decimals, Qt::KeyboardModifiers modifiers);

} // namespace app::scrub
