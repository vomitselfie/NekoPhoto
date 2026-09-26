#include "Style.h"
#include "LayersPanel.h"
#include "LayerStyleDialog.h"
#include "SmartFilterDialog.h"
#include "ImageConvert.h"
#include <QStandardItemModel>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QAbstractItemView>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include "Icons.h"
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

namespace {

const int thumbWidth = 44, thumbHeight = 34;

QPixmap thumbnailPixmap(const ImagePtr& image, bool group, double dpr) {
    QPixmap pixmap(int(thumbWidth * dpr), int(thumbHeight * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QRectF box(0, 0, thumbWidth, thumbHeight);
    if (group) {
        p.setPen(QColor(120, 120, 120));
        p.setBrush(QColor(90, 90, 90));
        p.drawRoundedRect(box.adjusted(6, 8, -6, -8), 3, 3);
        p.drawRect(QRectF(6, 6, 14, 5));
        return pixmap;
    }
    // Checkerboard under the thumbnail.
    for (int y = 0; y < thumbHeight; y += 6) for (int x = 0; x < thumbWidth; x += 6) p.fillRect(QRect(x, y, 6, 6), ((x / 6 + y / 6) % 2) ? QColor(200, 200, 200) : QColor(255, 255, 255));
    if (image) {
        QImage q = wrapImage(*image);
        double scale = std::min(thumbWidth / double(q.width()), thumbHeight / double(q.height()));
        QSizeF size(q.width() * scale, q.height() * scale);
        QRectF target((thumbWidth - size.width()) / 2, (thumbHeight - size.height()) / 2, size.width(), size.height());
        p.drawImage(target, q);
    }
    p.setPen(QColor(0, 0, 0, 80));
    p.setBrush(Qt::NoBrush);
    p.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
    return pixmap;
}

QPixmap maskPixmap(const GrayPtr& image, bool enabled, double dpr) {
    int w = 30, h = thumbHeight;
    QPixmap pixmap(int(w * dpr), int(h * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(QColor(60, 60, 60));
    QPainter p(&pixmap);
    if (image) {
        QImage q = toQImage(*image);
        if (q.width() == 1 && q.height() == 1) p.fillRect(QRect(0, 0, w, h), q.pixelColor(0, 0));
        else {
            double scale = std::min(w / double(q.width()), h / double(q.height()));
            QSizeF size(q.width() * scale, q.height() * scale);
            p.drawImage(QRectF((w - size.width()) / 2, (h - size.height()) / 2, size.width(), size.height()), q);
        }
    }
    if (!enabled) { p.setPen(QPen(QColor(220, 40, 40), 2)); p.drawLine(0, 0, w, h); p.drawLine(0, h, w, 0); }
    p.setPen(QColor(0, 0, 0, 80));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(0.5, 0.5, w - 1, h - 1));
    return pixmap;
}

QIcon eyeIcon(bool visible, double dpr, QColor color) {
    QPixmap pixmap(int(16 * dpr), int(16 * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    if (!visible) {
        // Hidden: an empty slot, so the difference reads at a glance (as Photoshop's empty box does).
        color.setAlpha(60);
        p.setPen(QPen(color, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(2.5, 2.5, 11, 11), 2, 2);
        return QIcon(pixmap);
    }
    p.setPen(QPen(color, 1.5));
    p.setBrush(Qt::NoBrush);
    QPainterPath eye;
    eye.moveTo(1.5, 8);
    eye.quadTo(8, 1.5, 14.5, 8);
    eye.quadTo(8, 14.5, 1.5, 8);
    p.drawPath(eye);
    p.setBrush(color);
    p.drawEllipse(QPointF(8, 8), 2.6, 2.6);
    return QIcon(pixmap);
}

// Child rows under a smart object: UserRole holds the smart object's id (so selecting one selects it), this role
// what the row is, and the next its entry's index.
constexpr int smartFilterRole = Qt::UserRole + 2, smartFilterIndexRole = Qt::UserRole + 3;
enum SmartFilterRow { NotSmartFilter = 0, SmartFilterHeader = 1, SmartFilterItem = 2 };
int smartFilterRow(const QTreeWidgetItem* item) { return item ? item->data(0, smartFilterRole).toInt() : NotSmartFilter; }

/// The stack's mask, small, for its thumbnail (the document's rect sampled; its tone past its bounds).
GrayPtr smartFilterMaskThumbnail(const SmartFilterStack& stack, int docW, int docH) {
    const double scale = std::min(1.0, 64.0 / std::max(1, std::max(docW, docH)));
    const int w = std::max(1, int(docW * scale)), h = std::max(1, int(docH * scale));
    auto out = std::make_shared<GrayImage>(w, h, stack.maskDefault);
    if (!stack.mask) return out;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int mx = int((x + 0.5) / scale) - stack.maskBounds.x, my = int((y + 0.5) / scale) - stack.maskBounds.y;
            if (mx >= 0 && my >= 0 && mx < stack.mask->width() && my < stack.mask->height()) out->at(x, y) = stack.mask->at(mx, my);
        }
    return out;
}

} // namespace

// ---- LayerTree ---------------------------------------------------------------------

LayerTree::LayerTree(EditorSession* session, QWidget* parent) : QTreeWidget(parent), session_(session) {
    setHeaderHidden(true);
    setRootIsDecorated(true);
    setIndentation(14);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setUniformRowHeights(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setMouseTracking(true);
}

QMimeData* LayerTree::mimeData(const QList<QTreeWidgetItem*>& items) const {
    QMimeData* mime = QTreeWidget::mimeData(items);
    if (!mime) mime = new QMimeData;
    QTreeWidgetItem* lead = currentItem() && items.contains(currentItem()) ? currentItem() : (items.isEmpty() ? nullptr : items.first());
    if (lead) mime->setData("application/x-nekophoto-layer", (QString::number(quintptr(session_)) + ":" + lead->data(0, Qt::UserRole).toString()).toUtf8());
    return mime;
}

void LayerTree::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) { event->ignore(); return; }   // files dropped here are the window's to open
    if (event->mimeData()->hasFormat("application/x-nekophoto-mask")) { event->acceptProposedAction(); return; }
    QTreeWidget::dragEnterEvent(event);
}

void LayerTree::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) { event->ignore(); return; }
    if (event->mimeData()->hasFormat("application/x-nekophoto-mask")) { event->acceptProposedAction(); return; }
    QTreeWidget::dragMoveEvent(event);
}

void LayerTree::mousePressEvent(QMouseEvent* event) {
    // Alt-click on a row clips it to the layer below (or releases it).
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::AltModifier) && !(event->modifiers() & Qt::ControlModifier)) {
        if (QTreeWidgetItem* item = itemAt(event->position().toPoint()); item && !smartFilterRow(item)) {
            QWidget* w = childAt(event->position().toPoint());
            while (w && w->property("layerId").isNull()) w = w->parentWidget();
            if (!(w && (w->property("eye").toBool() || w->property("mask").toBool()))) {
                session_->toggleClippingMask(item->data(0, Qt::UserRole).toString().toStdString());
                return;
            }
        }
    }
    QTreeWidget::mousePressEvent(event);
}

void LayerTree::dropEvent(QDropEvent* event) {
    QTreeWidgetItem* target = itemAt(event->position().toPoint());
    if (event->mimeData()->hasFormat("application/x-nekophoto-mask")) {
        event->ignore();
        if (!target) return;
        while (smartFilterRow(target)) target = target->parent();
        Uuid source = QString::fromUtf8(event->mimeData()->data("application/x-nekophoto-mask")).toStdString();
        session_->copyMask(source, target->data(0, Qt::UserRole).toString().toStdString());
        return;
    }
    // Onto a smart object's Smart Filters: onto the smart object.
    bool ontoSmartObject = false;
    while (smartFilterRow(target)) { target = target->parent(); ontoSmartObject = true; }
    dropDuplicates = event->modifiers() & Qt::AltModifier;
    QList<QTreeWidgetItem*> dragged = selectedItems();
    event->ignore();
    if (dragged.isEmpty()) return;
    // The current item leads; a multi-selection drops one at a time in tree order.
    QTreeWidgetItem* lead = currentItem() && dragged.contains(currentItem()) ? currentItem() : dragged.first();
    Uuid id = lead->data(0, Qt::UserRole).toString().toStdString();
    auto idOf = [](QTreeWidgetItem* item) -> std::optional<Uuid> { return item ? std::optional<Uuid>(item->data(0, Qt::UserRole).toString().toStdString()) : std::nullopt; };
    auto isGroup = [](QTreeWidgetItem* item) { return item && item->data(0, Qt::UserRole + 1).toBool(); };
    if (!target) { emit dropRequested(id, std::nullopt, std::nullopt, true); return; }
    QAbstractItemView::DropIndicatorPosition position = ontoSmartObject ? QAbstractItemView::OnItem : dropIndicatorPosition();
    std::optional<Uuid> parent = idOf(target->parent());
    // The list shows top first; "above" in the document is the item shown before.
    if (position == QAbstractItemView::OnItem) {
        if (isGroup(target)) { emit dropRequested(id, idOf(target), std::nullopt, false); return; }
        emit dropRequested(id, parent, idOf(target), false);
        return;
    }
    if (position == QAbstractItemView::AboveItem) { emit dropRequested(id, parent, idOf(target), false); return; }
    if (position == QAbstractItemView::BelowItem) {
        // Below the item shown: above the next sibling shown below it, or at the bottom of that parent.
        QTreeWidgetItem* container = target->parent();
        int index = container ? container->indexOfChild(target) : indexOfTopLevelItem(target);
        int count = container ? container->childCount() : topLevelItemCount();
        if (index + 1 < count) { QTreeWidgetItem* next = container ? container->child(index + 1) : topLevelItem(index + 1); emit dropRequested(id, parent, idOf(next), false); }
        else emit dropRequested(id, parent, std::nullopt, true);
        return;
    }
    emit dropRequested(id, std::nullopt, std::nullopt, true);
}

void LayerTree::mouseMoveEvent(QMouseEvent* event) {
    if (swiping) {
        QWidget* w = childAt(event->position().toPoint());
        while (w && w->property("layerId").isNull()) w = w->parentWidget();
        if (w && w->property("eye").toBool()) session_->setVisibilityInSwipe(w->property("layerId").toString().toStdString(), swipeVisible);
        return;
    }
    QTreeWidget::mouseMoveEvent(event);
}

void LayerTree::mouseReleaseEvent(QMouseEvent* event) {
    if (swiping) { emit swipeEnded(); return; }
    QTreeWidget::mouseReleaseEvent(event);
}

// ---- LayersPanel ---------------------------------------------------------------------

LayersPanel::LayersPanel(EditorSession* session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* appearance = new QHBoxLayout;
    blendCombo_ = new QComboBox;
    // Photoshop's order and groups; each item holds its mode, Pass Through (folders only) -1 at the top.
    blendCombo_->addItem(tr("Pass Through"), -1);
    for (int m : blendModeMenuOrder()) {
        if (m < 0) blendCombo_->insertSeparator(blendCombo_->count());
        else blendCombo_->addItem(QString::fromUtf8(blendModeName(BlendMode(m))), m);
    }
    blendCombo_->setToolTip(tr("Blend mode"));
    appearance->addWidget(blendCombo_, 1);
    opacitySlider_ = new QSlider(Qt::Horizontal);
    opacitySlider_->setRange(0, 100);
    opacitySlider_->setToolTip(tr("Opacity"));
    appearance->addWidget(opacitySlider_, 1);
    opacitySpin_ = new QSpinBox;
    opacitySpin_->setRange(0, 100);
    opacitySpin_->setSuffix("%");
    opacitySpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    opacitySpin_->setAlignment(Qt::AlignRight);
    opacitySpin_->setFixedWidth(52);
    appearance->addWidget(opacitySpin_);
    layout->addLayout(appearance);

    tree_ = new LayerTree(session_);
    layout->addWidget(tree_, 1);

    auto* footer = new QHBoxLayout;
    footer->setSpacing(2);
    auto button = [&](const QString& icon, const QString& tip, auto slot) {
        auto* b = new QToolButton;
        b->setIcon(toolIcon(icon, 18));
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, slot);
        footer->addWidget(b);
        return b;
    };
    button("square-plus", tr("New layer (Ctrl-click: below the current layer)"), [this] { session_->addBlankLayer(QApplication::keyboardModifiers() & Qt::ControlModifier); });
    button("folder-plus", tr("New folder"), [this] { session_->addGroup(); });
    button("mask", tr("Add layer mask (reveal all, or hide the selection)"), [this] { session_->addMaskFromSelection(true); });
    auto* adjust = button("sliders-horizontal", tr("New adjustment layer"), [] {});
    auto* adjustMenu = new QMenu(adjust);
    for (int i = 0; i < adjustmentKindCount; i++) {
        AdjustmentKind kind = AdjustmentKind(i);
        adjustMenu->addAction(QString::fromUtf8(adjustmentKindName(kind)), this, [this, kind] { session_->addAdjustmentLayer(kind); });
    }
    adjust->setMenu(adjustMenu);
    adjust->setPopupMode(QToolButton::InstantPopup);
    footer->addStretch();
    button("trash-2", tr("Delete the selected layers"), [this] { session_->deleteSelectedLayers(); });
    layout->addLayout(footer);

    connect(blendCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        const QVariant mode = blendCombo_->itemData(index);
        if (!mode.isValid()) return;
        if (mode.toInt() < 0) session_->setLayerBlendMode(BlendMode::Normal, true);
        else session_->setLayerBlendMode(BlendMode(mode.toInt()));
    });
    connect(blendCombo_, QOverload<int>::of(&QComboBox::highlighted), this, [this](int index) {
        const QVariant mode = blendCombo_->itemData(index);
        if (session_->canEditLayers() && mode.isValid() && mode.toInt() >= 0) session_->previewBlendMode(BlendMode(mode.toInt()));
    });
    blendCombo_->view()->installEventFilter(this);
    connect(opacitySlider_, &QSlider::sliderPressed, this, [this] { session_->beginOpacityEdit(); });
    connect(opacitySlider_, &QSlider::sliderReleased, this, [this] { session_->endOpacityEdit(); });
    connect(opacitySlider_, &QSlider::valueChanged, this, [this](int value) {
        if (opacitySpin_->value() != value) { QSignalBlocker b(opacitySpin_); opacitySpin_->setValue(value); }
        if (!rebuilding_) session_->setLayerOpacity(value / 100.0);
    });
    connect(opacitySpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (opacitySlider_->value() != value) { QSignalBlocker b(opacitySlider_); opacitySlider_->setValue(value); if (!rebuilding_) session_->setLayerOpacity(value / 100.0); }
    });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (rebuilding_) return;
        std::set<Uuid> ids;
        for (auto* item : tree_->selectedItems()) ids.insert(item->data(0, Qt::UserRole).toString().toStdString());
        std::optional<Uuid> primary;
        if (tree_->currentItem() && tree_->currentItem()->isSelected()) primary = tree_->currentItem()->data(0, Qt::UserRole).toString().toStdString();
        if (ids.size() == 1) session_->selectLayer(*ids.begin(), session_->isMaskSelected() && session_->activeLayerId() == *ids.begin());
        else session_->selectLayers(ids, primary);
    });
    auto folded = [this](QTreeWidgetItem* item, bool collapsed) {
        if (rebuilding_) return;
        const Uuid id = item->data(0, Qt::UserRole).toString().toStdString();
        if (item->data(0, Qt::UserRole + 1).toBool()) { session_->toggleGroupExpansion(id); return; }
        // A smart object's Smart Filters.
        if (collapsed) collapsedSmartFilters_.insert(id); else collapsedSmartFilters_.erase(id);
    };
    connect(tree_, &QTreeWidget::itemExpanded, this, [folded](QTreeWidgetItem* item) { folded(item, false); });
    connect(tree_, &QTreeWidget::itemCollapsed, this, [folded](QTreeWidgetItem* item) { folded(item, true); });
    connect(tree_, &LayerTree::dropRequested, this, [this](Uuid id, std::optional<Uuid> parent, std::optional<Uuid> above, bool atBottom) {
        // Dropped "above" a shown item means directly above it in the stack: the item shown becomes the one below.
        if (tree_->dropDuplicates) session_->duplicateLayerTo(id, parent, above, atBottom && !above);
        else session_->placeLayer(id, parent, above, atBottom && !above);
    });
    connect(tree_, &QWidget::customContextMenuRequested, this, &LayersPanel::showContextMenu);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        const Uuid id = item->data(0, Qt::UserRole).toString().toStdString();
        if (smartFilterRow(item) == SmartFilterItem) { editSmartFilter(id, item->data(0, smartFilterIndexRole).toInt(), false); return; }
        if (smartFilterRow(item)) return;
        startRename(id);
    });

    connect(tree_, &LayerTree::swipeEnded, this, &LayersPanel::finishSwipe);
    connect(session_, &EditorSession::layersChanged, this, [this] {
        // Rebuilding mid-swipe would destroy the eye button under the pointer and lose the release.
        if (tree_->swiping) { rebuildAfterSwipe_ = true; syncEyes(); return; }
        if (pendingRebuild_) return;
        pendingRebuild_ = true;
        QTimer::singleShot(0, this, [this] { pendingRebuild_ = false; rebuild(); });
    });
    rebuild();
}

QTreeWidgetItem* LayersPanel::itemFor(const Uuid& id) const {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : it->second;
}

QWidget* LayersPanel::makeRow(const Layer& layer, int depth, bool visible) {
    Q_UNUSED(depth);
    auto* row = new QWidget;
    row->setProperty("layerId", QString::fromStdString(layer.id));
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(2, 2, 4, 2);
    h->setSpacing(6);
    double dpr = devicePixelRatioF();

    auto* eye = new QToolButton;
    eye->setProperty("layerId", QString::fromStdString(layer.id));
    eye->setProperty("eye", true);
    eye->setAutoRaise(true);
    eye->setIcon(eyeIcon(layer.visible, dpr, palette().color(QPalette::Text)));
    eye->setIconSize(QSize(16, 16));
    eye->setToolTip(tr("Show or hide (drag across other eyes to set them too)"));
    eye->setFixedWidth(22);
    eye->installEventFilter(this);
    connect(eye, &QToolButton::pressed, this, [this, id = layer.id] {
        tree_->swiping = true;
        session_->beginVisibilitySwipe(id);
        const Layer* l = session_->document() ? session_->document()->find(id) : nullptr;
        tree_->swipeVisible = l ? l->visible : true;
    });
    h->addWidget(eye);

    bool isClipped = layer.maskSourceId.has_value();
    if (isClipped) {
        auto* arrow = new QLabel(QStringLiteral("↳"));
        arrow->setToolTip(tr("Clipped to the layer below"));
        h->addWidget(arrow);
    }
    auto* thumb = new QLabel;
    thumb->setPixmap(thumbnailPixmap(layer.asset ? layer.asset->thumbnail : nullptr, layer.isGroup, dpr));
    thumb->setFixedSize(thumbWidth, thumbHeight);
    if (layer.adjustment) { thumb->setPixmap(renderIcon("sliders-horizontal", palette().color(QPalette::Text), 20, dpr)); thumb->setAlignment(Qt::AlignCenter); thumb->setToolTip(QString::fromUtf8(adjustmentKindName(layer.adjustment->kind))); }
    bool activeImage = session_->activeLayerId() == layer.id && !session_->isMaskSelected();
    thumb->setStyleSheet(activeImage ? "border: 2px solid palette(highlight);" : "border: 2px solid transparent;");
    if (layer.isLiveText()) {
        thumb->setToolTip(tr("Text layer: double-click to edit the text"));
        thumb->setProperty("textLayer", QString::fromStdString(layer.id));
        thumb->installEventFilter(this);
    }
    h->addWidget(thumb);
    if (layer.mask) {
        auto* mask = new QLabel;
        mask->setPixmap(maskPixmap(layer.mask->asset.thumbnail, layer.mask->enabled, dpr));
        mask->setFixedSize(30, thumbHeight);
        bool activeMask = session_->activeLayerId() == layer.id && session_->isMaskSelected();
        mask->setStyleSheet(activeMask ? "border: 2px solid palette(highlight);" : "border: 2px solid transparent;");
        mask->setToolTip(layer.mask->linked ? tr("Layer mask (click to paint on it)") : tr("Layer mask, unlinked"));
        mask->setProperty("layerId", QString::fromStdString(layer.id));
        mask->setProperty("mask", true);
        mask->installEventFilter(this);
        h->addWidget(mask);
    }
    auto* name = new QLabel(QString::fromStdString(layer.name));
    name->setProperty("layerId", QString::fromStdString(layer.id));
    name->setProperty("name", true);
    if (!visible) name->setStyleSheet(hintStyle());
    if (layer.isGroup) { QFont f = name->font(); f.setBold(true); name->setFont(f); }
    h->addWidget(name, 1);
    if (layer.isLiveSmartObject()) {
        // The smart object badge: its contents open on a click (when NekoPhoto can redraw them).
        auto* badge = new QToolButton;
        badge->setAutoRaise(true);
        badge->setText(layer.smartObject->locked() ? QStringLiteral("◇") : QStringLiteral("◆"));
        const SmartObjectSource* source = nullptr;
        if (auto it = session_->document()->smartObjects.find(layer.smartObject->sourceId); it != session_->document()->smartObjects.end()) source = it->second.get();
        const QString file = source ? QString::fromStdString(source->fileName) : tr("its contents");
        badge->setToolTip(layer.smartObject->locked()
            ? tr("Smart object (%1), %2: it shows its stored preview and can be moved and scaled.").arg(file, QString::fromUtf8(smartObjectLockDescription(layer.smartObject->lock)))
            : tr("Smart object (%1): click to edit its contents.").arg(file));
        badge->setProperty("smartObjectBadge", true);
        const Uuid id = layer.id;
        connect(badge, &QToolButton::clicked, this, [this, id] { emit smartObjectContentsRequested(id); });
        h->addWidget(badge);
    }
    if (layer.opacity != 1 || layer.blendMode != BlendMode::Normal || (layer.isGroup && !layer.passThrough)) {
        auto* info = new QLabel(QStringLiteral("%1%").arg(int(std::round(layer.opacity * 100))));
        info->setStyleSheet(hintStyle(" font-size: 10px;"));
        h->addWidget(info);
    }
    return row;
}

void LayersPanel::rebuild() {
    rebuilding_ = true;
    tree_->clear();
    items_.clear();
    const auto& doc = session_->document();
    if (doc) {
        std::map<Uuid, QTreeWidgetItem*> parents;
        auto entries = hierarchyEntries(doc->layers, true, nullptr);
        const auto proxy = session_->filterMaskLayer();
        for (auto& e : entries) {
            if (proxy && e.layer->id == *proxy) continue;   // the filter mask being painted shows on its smart object
            QTreeWidgetItem* item = new QTreeWidgetItem;
            item->setData(0, Qt::UserRole, QString::fromStdString(e.layer->id));
            item->setData(0, Qt::UserRole + 1, e.layer->isGroup);
            item->setSizeHint(0, QSize(0, thumbHeight + 6));
            Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled;
            if (e.layer->isGroup) flags |= Qt::ItemIsDropEnabled;
            item->setFlags(flags);
            if (e.layer->parentId && parents.count(*e.layer->parentId)) parents[*e.layer->parentId]->addChild(item);
            else tree_->addTopLevelItem(item);
            parents[e.layer->id] = item;
            items_[e.layer->id] = item;
            tree_->setItemWidget(item, 0, makeRow(*e.layer, e.depth, e.visible));
            if (e.layer->smartObject) addSmartFilterRows(item, *e.layer);
        }
        tree_->invisibleRootItem()->setFlags(tree_->invisibleRootItem()->flags() | Qt::ItemIsDropEnabled);
        for (auto& [id, item] : items_)
            if (item->childCount() > 0) item->setExpanded(item->data(0, Qt::UserRole + 1).toBool() ? !session_->collapsedGroupIds.count(id) : !collapsedSmartFilters_.count(id));
        for (auto& id : session_->selectedLayerIds()) if (auto* item = itemFor(id)) item->setSelected(true);
        if (session_->activeLayerId()) if (auto* item = itemFor(*session_->activeLayerId())) { tree_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate); tree_->scrollToItem(item); }
    }
    syncAppearance();
    rebuilding_ = false;
}

QToolButton* LayersPanel::eyeButton(const Uuid& id) const {
    auto it = items_.find(id);
    if (it == items_.end()) return nullptr;
    // In view, so a pointer event there reaches it (a long layer list scrolls).
    tree_->scrollToItem(it->second);
    QWidget* row = tree_->itemWidget(it->second, 0);
    if (!row) return nullptr;
    for (auto* b : row->findChildren<QToolButton*>()) if (b->property("eye").toBool()) return b;
    return nullptr;
}

void LayersPanel::syncEyes() {
    const auto& doc = session_->document();
    if (!doc) return;
    for (auto& [id, item] : items_) {
        const Layer* l = doc->find(id);
        QWidget* row = tree_->itemWidget(item, 0);
        if (!l || !row) continue;
        for (auto* b : row->findChildren<QToolButton*>()) if (b->property("eye").toBool()) b->setIcon(eyeIcon(l->visible, devicePixelRatioF(), palette().color(QPalette::Text)));
    }
}

void LayersPanel::finishSwipe() {
    if (!tree_->swiping) return;
    tree_->swiping = false;
    session_->endVisibilitySwipe();
    if (rebuildAfterSwipe_) { rebuildAfterSwipe_ = false; rebuild(); }
}

void LayersPanel::syncAppearance() {
    const Layer* active = session_->activeLayer();
    bool enabled = active && !active->adjustment;
    blendCombo_->setEnabled(enabled);
    opacitySlider_->setEnabled(active != nullptr);
    opacitySpin_->setEnabled(active != nullptr);
    QSignalBlocker b1(blendCombo_), b2(opacitySlider_), b3(opacitySpin_);
    // Pass Through is a folder's alone.
    if (auto* model = qobject_cast<QStandardItemModel*>(blendCombo_->model()))
        if (auto* item = model->item(0)) item->setEnabled(active && active->isGroup);
    const int shown = !active ? int(BlendMode::Normal) : active->isGroup && active->passThrough ? -1 : int(active->blendMode);
    blendCombo_->setCurrentIndex(std::max(0, blendCombo_->findData(shown)));
    int opacity = active ? int(std::round(active->opacity * 100)) : 100;
    opacitySlider_->setValue(opacity);
    opacitySpin_->setValue(opacity);
}

void LayersPanel::startRename(const Uuid& id) {
    QTreeWidgetItem* item = itemFor(id);
    if (!item) return;
    QWidget* row = tree_->itemWidget(item, 0);
    if (!row) return;
    QLabel* label = nullptr;
    for (auto* l : row->findChildren<QLabel*>()) if (l->property("name").toBool()) label = l;
    if (!label) return;
    auto* edit = new QLineEdit(label->text(), row);
    edit->setGeometry(label->geometry());
    edit->selectAll();
    edit->show();
    edit->setFocus();
    auto finish = [this, edit, id, done = std::make_shared<bool>(false)](bool apply) {
        if (*done) return;
        *done = true;
        QString text = edit->text();
        edit->deleteLater();
        if (apply) session_->renameLayer(id, text);
    };
    connect(edit, &QLineEdit::editingFinished, this, [finish] { finish(true); });
    edit->installEventFilter(this);
    edit->setProperty("renameEditor", true);
}

bool LayersPanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonDblClick) {
        QString textLayer = watched->property("textLayer").toString();
        if (!textLayer.isEmpty()) {
            Uuid id = textLayer.toStdString();
            if (session_->activeLayerId() != id) session_->selectLayer(id);
            session_->requestTextEdit(id);
            return true;
        }
    }
    if (watched == blendCombo_->view() && event->type() == QEvent::Hide) session_->previewBlendMode(std::nullopt);
    // The eye button owns the pointer during a swipe: moves toggle the eye under the pointer, the release ends it.
    if (auto* eye = qobject_cast<QWidget*>(watched); eye && eye->property("eye").toBool() && tree_->swiping) {
        if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            QWidget* w = tree_->viewport()->childAt(tree_->viewport()->mapFromGlobal(mouse->globalPosition().toPoint()));
            while (w && w->property("layerId").isNull()) w = w->parentWidget();
            if (w && w->property("eye").toBool()) session_->setVisibilityInSwipe(w->property("layerId").toString().toStdString(), tree_->swipeVisible);
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) { finishSwipe(); return true; }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto* w = qobject_cast<QWidget*>(watched);
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (w && w->property("smartFilterMask").toBool()) {
            const Uuid id = w->property("smartObject").toString().toStdString();
            QString error;
            bool ok = true;
            if (mouse->modifiers() & Qt::ShiftModifier) {
                auto stack = session_->smartFilters(id);
                ok = stack && session_->smartFilterMask(id, stack->maskEnabled ? EditorSession::FilterMaskAction::Disable : EditorSession::FilterMaskAction::Enable, &error);
            } else if (mouse->modifiers() & Qt::AltModifier) ok = session_->beginFilterMaskEdit(id, !session_->filterMaskShown(), &error);
            else ok = session_->beginFilterMaskEdit(id, false, &error);
            if (!ok && !error.isEmpty()) QMessageBox::warning(this, tr("Smart Filters"), error);
            return true;
        }
        if (w && w->property("mask").toBool()) {
            if (mouse->modifiers() & Qt::AltModifier) {
                // Alt-drag a mask onto another layer to copy it there.
                auto* drag = new QDrag(w);
                auto* mime = new QMimeData;
                mime->setData("application/x-nekophoto-mask", w->property("layerId").toString().toUtf8());
                drag->setMimeData(mime);
                if (auto* label = qobject_cast<QLabel*>(w)) drag->setPixmap(label->pixmap());
                drag->exec(Qt::CopyAction);
                return true;
            }
            session_->selectLayer(w->property("layerId").toString().toStdString(), true);
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress && watched->property("renameEditor").toBool()) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) { qobject_cast<QLineEdit*>(watched)->deleteLater(); return true; }
    }
    return QWidget::eventFilter(watched, event);
}

void LayersPanel::showContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = tree_->itemAt(pos);
    if (!item) return;
    if (smartFilterRow(item)) { showSmartFilterMenu(item, tree_->viewport()->mapToGlobal(pos)); return; }
    Uuid id = item->data(0, Qt::UserRole).toString().toStdString();
    if (session_->activeLayerId() != id) session_->selectLayer(id);
    const Layer* layer = session_->document() ? session_->document()->find(id) : nullptr;
    if (!layer) return;
    QMenu menu(this);
    if (layer->isLiveText()) menu.addAction(tr("Edit Text…"), this, [this, id] { session_->requestTextEdit(id); });
    menu.addAction(tr("Rename…"), this, [this, id] { startRename(id); });
    if (session_->canStyleLayer(id)) {
        menu.addAction(tr("Layer Style…"), this, [this, id] { LayerStyleDialog(session_, id, this).exec(); });
        if (session_->activeLayerHasStyle()) {
            menu.addAction(tr("Copy Layer Style"), this, [this] { session_->copyLayerStyle(); });
            menu.addAction(tr("Clear Layer Style"), this, [this] { session_->clearLayerStyle(); });
        }
        if (session_->canPasteLayerStyle()) menu.addAction(tr("Paste Layer Style"), this, [this] { session_->pasteLayerStyle(); });
    }

    if (session_->smartFilters(id)) {
        QAction* clear = menu.addAction(tr("Clear Smart Filters"), this, [this, id] { QString e; if (!session_->clearSmartFilters(id, &e)) QMessageBox::warning(this, tr("Smart Filters"), e); });
        clear->setEnabled(session_->canEditSmartFilters(id));
        menu.addSeparator();
    }
    menu.addAction(tr("Duplicate Layer"), this, [this] { session_->duplicateActiveLayer(); });
    menu.addAction(tr("Delete Layer"), this, [this, id] { session_->deleteLayer(id); });
    menu.addSeparator();
    if (!layer->isGroup) {
        QAction* clip = menu.addAction(layer->maskSourceId ? tr("Release Clipping Mask") : tr("Create Clipping Mask"), this, [this, id] { session_->toggleClippingMask(id); });
        clip->setEnabled(session_->canToggleClippingMask(id));
        menu.addAction(tr("Merge Down"), this, [this] { session_->mergeDown(); });
        menu.addSeparator();
    }
    if (layer->mask) {
        menu.addAction(layer->mask->enabled ? tr("Disable Layer Mask") : tr("Enable Layer Mask"), this, [this] { session_->toggleLayerMask(); });
        menu.addAction(layer->mask->linked ? tr("Unlink Layer Mask") : tr("Link Layer Mask"), this, [this, id] { session_->toggleMaskLink(id); });
        menu.addAction(tr("Invert Mask"), this, [this] { session_->invertMask(); });
        if (!layer->isGroup) menu.addAction(tr("Apply Layer Mask"), this, [this] { session_->applyMask(); });
        menu.addAction(tr("Delete Layer Mask"), this, [this] { session_->deleteLayerMask(); });
    } else {
        menu.addAction(tr("Add Reveal-All Mask"), this, [this] { session_->addMaskFromSelection(true); });
        menu.addAction(tr("Add Hide-All Mask"), this, [this] { session_->addMaskFromSelection(false); });
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

// ---- Smart Filters -------------------------------------------------------------------------------

void LayersPanel::addSmartFilterRows(QTreeWidgetItem* parent, const Layer& layer) {
    auto stack = session_->smartFilters(layer.id);
    if (!stack) return;
    const bool editable = session_->canEditSmartFilters(layer.id);
    auto add = [&](int kind, int index, QWidget* row) {
        auto* item = new QTreeWidgetItem;
        item->setData(0, Qt::UserRole, QString::fromStdString(layer.id));
        item->setData(0, Qt::UserRole + 1, false);
        item->setData(0, smartFilterRole, kind);
        item->setData(0, smartFilterIndexRole, index);
        item->setSizeHint(0, QSize(0, kind == SmartFilterHeader ? thumbHeight + 6 : 26));
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);   // not dragged, nothing dropped into it
        parent->addChild(item);
        tree_->setItemWidget(item, 0, row);
    };
    add(SmartFilterHeader, -1, makeSmartFilterHeader(layer, *stack, editable));
    for (int i = int(stack->entries.size()) - 1; i >= 0; i--) add(SmartFilterItem, i, makeSmartFilterEntry(layer, *stack, i, editable));
}

QWidget* LayersPanel::makeSmartFilterHeader(const Layer& layer, const SmartFilterStack& stack, bool editable) {
    const double dpr = devicePixelRatioF();
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(2, 2, 4, 2);
    h->setSpacing(6);
    auto* eye = new QToolButton;
    eye->setAutoRaise(true);
    eye->setIcon(eyeIcon(stack.enabled, dpr, palette().color(QPalette::Text)));
    eye->setIconSize(QSize(16, 16));
    eye->setFixedWidth(22);
    eye->setProperty("smartFilterEye", true);
    eye->setToolTip(tr("Turn all Smart Filters on or off"));
    eye->setEnabled(editable);
    connect(eye, &QToolButton::clicked, this, [this, id = layer.id, on = !stack.enabled] {
        QString e;
        if (!session_->setSmartFilterEnabled(id, -1, on, &e)) QMessageBox::warning(this, tr("Smart Filters"), e);
    });
    h->addWidget(eye);
    auto* mask = new QLabel;
    const Document* doc = session_->document() ? &*session_->document() : nullptr;
    mask->setPixmap(maskPixmap(smartFilterMaskThumbnail(stack, doc ? doc->width : 1, doc ? doc->height : 1), stack.maskEnabled, dpr));
    mask->setFixedSize(30, thumbHeight);
    const bool painting = session_->filterMaskOwner() == layer.id;
    mask->setStyleSheet(painting ? "border: 2px solid palette(highlight);" : "border: 2px solid transparent;");
    mask->setToolTip(editable ? tr("Filter mask: click to paint on it, Alt-click to show it, Shift-click to turn it off or on")
                              : tr("Filter mask (these Smart Filters cannot be changed here)"));
    mask->setProperty("smartFilterMask", true);
    mask->setProperty("smartObject", QString::fromStdString(layer.id));
    mask->setEnabled(editable);
    if (editable) mask->installEventFilter(this);
    h->addWidget(mask);
    auto* name = new QLabel(painting && session_->filterMaskShown() ? tr("Smart Filters (mask shown)") : tr("Smart Filters"));
    if (!stack.enabled) name->setStyleSheet(hintStyle());
    h->addWidget(name, 1);
    return row;
}

QWidget* LayersPanel::makeSmartFilterEntry(const Layer& layer, const SmartFilterStack& stack, int index, bool editable) {
    const SmartFilterEntry& entry = stack.entries[size_t(index)];
    const bool drawn = !std::holds_alternative<std::monostate>(entry.parameters);
    const double dpr = devicePixelRatioF();
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(2, 0, 4, 0);
    h->setSpacing(6);
    auto* eye = new QToolButton;
    eye->setAutoRaise(true);
    eye->setIcon(eyeIcon(entry.enabled, dpr, palette().color(QPalette::Text)));
    eye->setIconSize(QSize(16, 16));
    eye->setFixedWidth(22);
    eye->setProperty("smartFilterEye", true);
    eye->setToolTip(tr("Turn this Smart Filter on or off"));
    eye->setEnabled(editable);
    connect(eye, &QToolButton::clicked, this, [this, id = layer.id, index, on = !entry.enabled] {
        QString e;
        if (!session_->setSmartFilterEnabled(id, index, on, &e)) QMessageBox::warning(this, tr("Smart Filters"), e);
    });
    h->addWidget(eye);
    h->addSpacing(36);
    QString text = QString::fromStdString(entry.name).replace(QStringLiteral("&&"), QStringLiteral("&"));
    if (text.isEmpty()) text = tr("Smart Filter");
    auto* name = new QLabel(text);
    if (!entry.enabled || !stack.enabled || !drawn) name->setStyleSheet(hintStyle());
    name->setToolTip(!drawn ? tr("NekoPhoto does not draw this filter: the layer keeps the preview the file carried, and it cannot be edited here.")
                     : editable ? tr("Double-click to change its settings") : tr("These Smart Filters cannot be changed here."));
    name->setEnabled(drawn);
    h->addWidget(name, 1);
    if (entry.opacity < 1 || entry.blend != BlendMode::Normal) {
        auto* info = new QLabel(QStringLiteral("%1%").arg(int(std::round(entry.opacity * 100))));
        info->setStyleSheet(hintStyle(" font-size: 10px;"));
        h->addWidget(info);
    }
    auto* blending = new QToolButton;
    blending->setAutoRaise(true);
    blending->setIcon(toolIcon("sliders-horizontal", 14));
    blending->setIconSize(QSize(14, 14));
    blending->setToolTip(tr("Blending Options (opacity and mode)"));
    blending->setEnabled(editable && drawn);
    connect(blending, &QToolButton::clicked, this, [this, id = layer.id, index] { editSmartFilter(id, index, true); });
    h->addWidget(blending);
    return row;
}

void LayersPanel::editSmartFilter(const Uuid& id, int index, bool blending) {
    if (!SmartFilterDialog::canEdit(session_, id, index)) {
        QMessageBox::information(this, tr("Smart Filters"),
                                 tr("This Smart Filter cannot be edited here: NekoPhoto does not draw it (or one beside it), or the smart object is locked."));
        return;
    }
    SmartFilterDialog dialog(session_, id, index, blending ? SmartFilterDialog::Page::Blending : SmartFilterDialog::Page::Settings, this);
    dialog.exec();
}

void LayersPanel::showSmartFilterMenu(QTreeWidgetItem* item, const QPoint& globalPos) {
    const Uuid id = item->data(0, Qt::UserRole).toString().toStdString();
    auto stack = session_->smartFilters(id);
    if (!stack) return;
    const bool editable = session_->canEditSmartFilters(id);
    auto warn = [this](bool ok, const QString& error) { if (!ok && !error.isEmpty()) QMessageBox::warning(this, tr("Smart Filters"), error); };
    QMenu menu(this);
    if (smartFilterRow(item) == SmartFilterItem) {
        const int index = item->data(0, smartFilterIndexRole).toInt();
        const int count = int(stack->entries.size());
        if (index < 0 || index >= count) return;
        const SmartFilterEntry& entry = stack->entries[size_t(index)];
        const bool canEdit = SmartFilterDialog::canEdit(session_, id, index);
        menu.addAction(tr("Edit Smart Filter…"), this, [this, id, index] { editSmartFilter(id, index, false); })->setEnabled(canEdit);
        menu.addAction(tr("Edit Smart Filter Blending Options…"), this, [this, id, index] { editSmartFilter(id, index, true); })->setEnabled(canEdit);
        menu.addAction(entry.enabled ? tr("Disable Smart Filter") : tr("Enable Smart Filter"), this, [this, id, index, on = !entry.enabled, warn] {
            QString e;
            warn(session_->setSmartFilterEnabled(id, index, on, &e), e);
        })->setEnabled(editable);
        menu.addSeparator();
        // The panel lists the last applied first: up in the list is later in the stack.
        menu.addAction(tr("Move Up"), this, [this, id, index, warn] { QString e; warn(session_->moveSmartFilter(id, index, index + 1, &e), e); })->setEnabled(editable && index + 1 < count);
        menu.addAction(tr("Move Down"), this, [this, id, index, warn] { QString e; warn(session_->moveSmartFilter(id, index, index - 1, &e), e); })->setEnabled(editable && index > 0);
        menu.addAction(tr("Delete Smart Filter"), this, [this, id, index, warn] { QString e; warn(session_->removeSmartFilter(id, index, &e), e); })->setEnabled(editable);
        menu.addSeparator();
    } else {
        using A = EditorSession::FilterMaskAction;
        const A toggle = stack->maskEnabled ? A::Disable : A::Enable;
        menu.addAction(stack->maskEnabled ? tr("Disable Filter Mask") : tr("Enable Filter Mask"), this, [this, id, warn, toggle] {
            QString e;
            warn(session_->smartFilterMask(id, toggle, &e), e);
        })->setEnabled(editable);
        menu.addAction(tr("Invert Filter Mask"), this, [this, id, warn] { QString e; warn(session_->smartFilterMask(id, A::Invert, &e), e); })->setEnabled(editable);
        menu.addAction(tr("Delete Filter Mask"), this, [this, id, warn] { QString e; warn(session_->smartFilterMask(id, A::Delete, &e), e); })->setEnabled(editable);
        menu.addSeparator();
        menu.addAction(stack->enabled ? tr("Disable Smart Filters") : tr("Enable Smart Filters"), this, [this, id, warn, on = !stack->enabled] {
            QString e;
            warn(session_->setSmartFilterEnabled(id, -1, on, &e), e);
        })->setEnabled(editable);
    }
    menu.addAction(tr("Clear Smart Filters"), this, [this, id, warn] { QString e; warn(session_->clearSmartFilters(id, &e), e); })->setEnabled(editable);
    menu.exec(globalPos);
}

} // namespace app
