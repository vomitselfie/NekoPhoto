// The Layers panel: blend mode and opacity for the active layer, the layer
// tree with eye toggles, thumbnails, mask thumbnails and drag reordering, and
// the footer buttons.
#pragma once
#include "nekophoto/smartfilter.h"
#include "nekophoto/uuid.h"
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QTreeWidget>
#include <QWidget>
#include <map>
#include <set>

class QToolButton;

namespace app {

class EditorSession;

class LayerTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit LayerTree(EditorSession* session, QWidget* parent = nullptr);
signals:
    void dropRequested(nekophoto::Uuid id, std::optional<nekophoto::Uuid> parent, std::optional<nekophoto::Uuid> above, bool atBottom);
    void swipeEnded();
    /// A Smart Filter row of smart object `id` dragged from running place `from` to `to`.
    void smartFilterMoveRequested(nekophoto::Uuid id, int from, int to);
public:
    /// Drops a dragged Smart Filter (entry `from` of smart object `id`) above or below `target`: only onto another
    /// entry row of the same stack, else refused (false). The drag's release and the debug hook both come here.
    bool dropSmartFilter(QTreeWidgetItem* target, bool above, const nekophoto::Uuid& id, int from);
    /// The entry row `index` of smart object `id`'s stack, if shown.
    QTreeWidgetItem* smartFilterItem(const nekophoto::Uuid& id, int index) const;
    bool dropDuplicates = false;
protected:
    void dropEvent(QDropEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
private:
    /// Where a Smart Filter drag at `pos` would land: the entry row and whether above it; null when refused.
    QTreeWidgetItem* smartFilterDropAt(const QMimeData* mime, QPoint pos, bool& above) const;
    EditorSession* session_;
    // A press on a Smart Filter entry row that may become a drag (the rows are not selectable, so Qt's own drag
    // never starts from them).
    std::optional<std::pair<nekophoto::Uuid, int>> filterPress_;
    QPoint filterPressAt_;
    QRect filterIndicator_;   // the drop line while a Smart Filter is dragged
public:
    bool swiping = false;
    bool swipeVisible = true;
};

class LayersPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayersPanel(EditorSession* session, QWidget* parent = nullptr);

signals:
    /// The smart object badge was clicked: open that layer's contents.
    void smartObjectContentsRequested(const nekophoto::Uuid& id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();
    void syncAppearance();
    /// Eye icons refreshed in place (the rows are kept while a swipe is in progress).
    void syncEyes();
public:
    /// The eye button of a layer's row, for tests that synthesise the swipe gesture.
    QToolButton* eyeButton(const nekophoto::Uuid& id) const;
    /// Test hook: drops Smart Filter `from` of `id` above or below the row of entry `onto` (any smart object's) as a
    /// drag's release would; false when refused.
    /// The active layer's context menu, as a right-click on its row opens it (for --dialog layers-menu).
    void showActiveLayerMenu();
    bool dropSmartFilterForTest(const nekophoto::Uuid& id, int from, const nekophoto::Uuid& ontoId, int onto, bool above);
private:
    void finishSwipe();
    /// The opacity (with `fill`, the Fill) a click, a key, the field or the end of a drag set: layers.set, one undo step.
    void commitOpacity(double opacity, bool fill = false);
    /// The Opacity or Fill row: a slider and a field kept together, a drag one undo step (layers.set at the release).
    void connectOpacityRow(QSlider* slider, QSpinBox* spin, bool fill);
    /// The mask button (with a selection, Reveal Selection; Alt-click, Hide Selection; without one, Reveal All or Hide
    /// All) and, `whole`, the row menu's Add Reveal-All / Hide-All Mask: layers.mask.
    void addMask(bool revealing, bool whole = false);
    QWidget* makeRow(const nekophoto::Layer& layer, int depth, bool visible);
    /// A smart object's Smart Filters as child rows of its item: the stack's row (its mask, its switch), then each
    /// entry, last applied first as in Photoshop. They are not layers: their data names the smart object.
    void addSmartFilterRows(QTreeWidgetItem* item, const nekophoto::Layer& layer);
    QWidget* makeSmartFilterHeader(const nekophoto::Layer& layer, const nekophoto::SmartFilterStack& stack, bool editable);
    QWidget* makeSmartFilterEntry(const nekophoto::Layer& layer, const nekophoto::SmartFilterStack& stack, int index, bool editable);
    void showSmartFilterMenu(QTreeWidgetItem* item, const QPoint& globalPos);
    void editSmartFilter(const nekophoto::Uuid& id, int index, bool blending);
    void showContextMenu(const QPoint& pos);
    void startRename(const nekophoto::Uuid& id);
    QTreeWidgetItem* itemFor(const nekophoto::Uuid& id) const;

    EditorSession* session_;
    QComboBox* blendCombo_;
    QSlider* opacitySlider_;
    QSpinBox* opacitySpin_;
    QSlider* fillSlider_;
    QSpinBox* fillSpin_;
    LayerTree* tree_;
    std::map<nekophoto::Uuid, QTreeWidgetItem*> items_;
    std::set<nekophoto::Uuid> collapsedSmartFilters_;   // smart objects whose Smart Filters are folded away
    bool rebuilding_ = false;
    bool pendingRebuild_ = false;
    bool rebuildAfterSwipe_ = false;
};

} // namespace app
