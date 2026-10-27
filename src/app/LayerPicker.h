// The Layer Picker (View > Layer Picker, Alt+A): the layers under the pointer, as the Move tool's right-click list in
// Photoshop, beside the pointer and driven from the keyboard (RingController.h). It lists the layers whose visible
// pixels cover the pixel under the pointer, top of the stack first (EditorSession::layersAt: a hidden layer, a
// transparent pixel, a mask hiding it or a clipping base not showing there leave a layer out; adjustment layers and
// groups have no pixels of their own and are not listed, a layer's group is named beside it). By the compass rule,
// F and R go up the stack and S and V down it, and the layer stepped to is the active layer at once (the Layers panel
// and the options bar follow). Esc puts back the layer that was active when it opened; Alt+A again, Enter, D, a click
// on the canvas, another tool or the idle timeout keep the choice. Choosing a layer is not an undo step.
#pragma once
#include "RingController.h"
#include "RingOverlay.h"
#include "nekophoto/uuid.h"
#include <QPixmap>
#include <QString>
#include <QWidget>
#include <set>
#include <vector>

class QMenu;

namespace app {

namespace motion { class Tween; }
class CommandRegistry;

/// What the picker paints: a column of the layers, the active one lit, the keys' hints above and below. Transparent
/// for the mouse, never focused. It pops open as the rings do (RingOverlay.h); the lit bar glides from row to row as
/// the keys step, the column scrolls smoothly to keep it in view, and on Esc the bar glides back to the layer put back
/// before the picker fades.
class LayerPickerOverlay : public RingOverlay {
    Q_OBJECT
public:
    struct Entry {
        QPixmap thumbnail;
        QString name;
        QString hint;   // the blend mode and opacity, and the group it is in
        bool active = false;
    };
    explicit LayerPickerOverlay(QWidget* parent);
    /// The rows (top of the stack first), or none with `empty` saying why.
    void setEntries(std::vector<Entry> entries, const QString& empty);
    const std::vector<Entry>& entries() const { return entries_; }
    /// Esc: the bar goes back to row `index` (-1: no listed row; it fades) and then the picker fades.
    void settleOn(int index);
    /// Where entry `index` is drawn once the column is still (widget coordinates; empty when it is scrolled out).
    QRect entryRect(int index) const;
    /// The size the rows ask for.
    QSize preferredSize() const;
    static constexpr int thumbWidth = 40, thumbHeight = 30;
    void finishMotion() override;
    // For tests: the row the bar is at now (between two while it glides; -1 none), and whether the column scrolls.
    double highlightRow() const;
    bool scrolling() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void hidden() override;

private:
    /// The first row shown: the column shows at most `maxRows`, keeping the active row in view.
    int firstShown() const;
    int shownCount() const;
    int activeIndex() const;
    /// The bar and the column's top row as painted now (rows, fractional while they move).
    double shownBar() const;
    double shownFirst() const;
    static constexpr int maxRows = 9, rowHeight = 40, hintHeight = 22, width_ = 248, captionHeight = 34;
    std::vector<Entry> entries_;
    QString empty_;
    motion::Tween* bar_;
    motion::Tween* scroll_;
    double barFrom_ = -1, barTo_ = -1, firstFrom_ = 0, firstTo_ = 0;
};

class LayerPicker : public RingController {
    Q_OBJECT
public:
    explicit LayerPicker(MainWindow* window);
    /// View > Layer Picker (view.layerPicker, Alt+A).
    void registerCommands(CommandRegistry& registry, QMenu* viewMenu);
    LayerPickerOverlay* overlay() const { return pickerOverlay_; }
    /// The layers listed, top of the stack first, and the document point they are under.
    const std::vector<nekophoto::Uuid>& layers() const { return layers_; }
    QPointF point() const { return point_; }
    /// Makes the layer `by` rows up (-1) or down (+1) the list active, as F/R and S/V do, or says why it cannot.
    void stepBy(int by);

protected:
    bool mayOpen() const override;
    void opening() override;
    void closed(bool cancelled) override;
    void refresh() override;
    char keyOf(const QKeyEvent* e) const override;
    void press(char key, bool autoRepeat) override;
    bool wantsWheel() const override { return true; }
    void wheelStep(bool more) override { stepBy(more ? -1 : 1); }
    void canvasPressed() override { close(); }

private:
    /// Why no layer can be chosen now (a stroke, a transform, a gradient or a warp under way), or empty.
    QString busy() const;

    LayerPickerOverlay* pickerOverlay_;
    std::vector<nekophoto::Uuid> layers_;
    QPointF point_;
    /// The selection when it opened, which Esc puts back.
    std::optional<nekophoto::Uuid> activeBefore_;
    std::set<nekophoto::Uuid> selectedBefore_;
    bool maskBefore_ = false;
};

} // namespace app
