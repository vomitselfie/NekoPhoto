#include "Style.h"
#include "LayersPanel.h"
#include "ImageConvert.h"
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
    if (!visible) color.setAlpha(70);
    p.setPen(QPen(color, 1.5));
    p.setBrush(Qt::NoBrush);
    // An almond-shaped eye with a pupil; hollow when hidden.
    QPainterPath eye;
    eye.moveTo(1.5, 8);
    eye.quadTo(8, 1.5, 14.5, 8);
    eye.quadTo(8, 14.5, 1.5, 8);
    p.drawPath(eye);
    if (visible) { p.setBrush(color); p.drawEllipse(QPointF(8, 8), 2.6, 2.6); }
    return QIcon(pixmap);
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
    if (lead) mime->setData("application/x-compositor-linux-layer", (QString::number(quintptr(session_)) + ":" + lead->data(0, Qt::UserRole).toString()).toUtf8());
    return mime;
}

void LayerTree::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat("application/x-compositor-linux-mask")) { event->acceptProposedAction(); return; }
    QTreeWidget::dragEnterEvent(event);
}

void LayerTree::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasFormat("application/x-compositor-linux-mask")) { event->acceptProposedAction(); return; }
    QTreeWidget::dragMoveEvent(event);
}

void LayerTree::mousePressEvent(QMouseEvent* event) {
    // Alt-click on a row clips it to the layer below (or releases it).
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::AltModifier) && !(event->modifiers() & Qt::ControlModifier)) {
        if (QTreeWidgetItem* item = itemAt(event->position().toPoint())) {
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
    if (event->mimeData()->hasFormat("application/x-compositor-linux-mask")) {
        event->ignore();
        if (!target) return;
        Uuid source = QString::fromUtf8(event->mimeData()->data("application/x-compositor-linux-mask")).toStdString();
        session_->copyMask(source, target->data(0, Qt::UserRole).toString().toStdString());
        return;
    }
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
    QAbstractItemView::DropIndicatorPosition position = dropIndicatorPosition();
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
    if (swiping) { swiping = false; session_->endVisibilitySwipe(); return; }
    QTreeWidget::mouseReleaseEvent(event);
}

// ---- LayersPanel ---------------------------------------------------------------------

LayersPanel::LayersPanel(EditorSession* session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* appearance = new QHBoxLayout;
    blendCombo_ = new QComboBox;
    for (int i = 0; i < blendModeCount; i++) blendCombo_->addItem(QString::fromUtf8(blendModeName(BlendMode(i))));
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
    button("square-plus", tr("New layer"), [this] { session_->addBlankLayer(); });
    button("folder-plus", tr("New folder"), [this] { session_->addGroup(); });
    button("mask", tr("Add layer mask (reveal all, or hide the selection)"), [this] { session_->addMaskFromSelection(true); });
    auto* adjust = button("sliders-horizontal", tr("New adjustment layer"), [] {});
    auto* adjustMenu = new QMenu(adjust);
    for (int i = 0; i < 6; i++) {
        AdjustmentKind kind = AdjustmentKind(i);
        adjustMenu->addAction(QString::fromUtf8(adjustmentKindName(kind)), this, [this, kind] { session_->addAdjustmentLayer(kind); });
    }
    adjust->setMenu(adjustMenu);
    adjust->setPopupMode(QToolButton::InstantPopup);
    footer->addStretch();
    button("trash-2", tr("Delete the selected layers"), [this] { session_->deleteSelectedLayers(); });
    layout->addLayout(footer);

    connect(blendCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) { session_->setLayerBlendMode(BlendMode(index)); });
    connect(blendCombo_, QOverload<int>::of(&QComboBox::highlighted), this, [this](int index) { if (session_->canEditLayers()) session_->previewBlendMode(BlendMode(index)); });
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
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { if (!rebuilding_) session_->toggleGroupExpansion(item->data(0, Qt::UserRole).toString().toStdString()); });
    connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) { if (!rebuilding_) session_->toggleGroupExpansion(item->data(0, Qt::UserRole).toString().toStdString()); });
    connect(tree_, &LayerTree::dropRequested, this, [this](Uuid id, std::optional<Uuid> parent, std::optional<Uuid> above, bool atBottom) {
        // Dropped "above" a shown item means directly above it in the stack: the item shown becomes the one below.
        if (tree_->dropDuplicates) session_->duplicateLayerTo(id, parent, above, atBottom && !above);
        else session_->placeLayer(id, parent, above, atBottom && !above);
    });
    connect(tree_, &QWidget::customContextMenuRequested, this, &LayersPanel::showContextMenu);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) { startRename(item->data(0, Qt::UserRole).toString().toStdString()); });

    connect(session_, &EditorSession::layersChanged, this, [this] {
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
    if (!layer.isGroup && (layer.opacity != 1 || layer.blendMode != BlendMode::Normal)) {
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
        for (auto& e : entries) {
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
        }
        tree_->invisibleRootItem()->setFlags(tree_->invisibleRootItem()->flags() | Qt::ItemIsDropEnabled);
        for (auto& [id, item] : items_) if (item->childCount() > 0) item->setExpanded(!session_->collapsedGroupIds.count(id));
        for (auto& id : session_->selectedLayerIds()) if (auto* item = itemFor(id)) item->setSelected(true);
        if (session_->activeLayerId()) if (auto* item = itemFor(*session_->activeLayerId())) { tree_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate); tree_->scrollToItem(item); }
    }
    syncAppearance();
    rebuilding_ = false;
}

void LayersPanel::syncAppearance() {
    const Layer* active = session_->activeLayer();
    bool enabled = active && !active->isGroup && !active->adjustment;
    blendCombo_->setEnabled(enabled);
    opacitySlider_->setEnabled(active && !active->isGroup);
    opacitySpin_->setEnabled(active && !active->isGroup);
    QSignalBlocker b1(blendCombo_), b2(opacitySlider_), b3(opacitySpin_);
    blendCombo_->setCurrentIndex(active ? int(active->blendMode) : 0);
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
    if (watched == blendCombo_->view() && event->type() == QEvent::Hide) session_->previewBlendMode(std::nullopt);
    if (event->type() == QEvent::MouseButtonPress) {
        auto* w = qobject_cast<QWidget*>(watched);
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (w && w->property("mask").toBool()) {
            if (mouse->modifiers() & Qt::AltModifier) {
                // Alt-drag a mask onto another layer to copy it there.
                auto* drag = new QDrag(w);
                auto* mime = new QMimeData;
                mime->setData("application/x-compositor-linux-mask", w->property("layerId").toString().toUtf8());
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
    Uuid id = item->data(0, Qt::UserRole).toString().toStdString();
    if (session_->activeLayerId() != id) session_->selectLayer(id);
    const Layer* layer = session_->document() ? session_->document()->find(id) : nullptr;
    if (!layer) return;
    QMenu menu(this);
    menu.addAction(tr("Rename…"), this, [this, id] { startRename(id); });
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

} // namespace app
