#include "ChannelsPanel.h"
#include "ChannelDialogs.h"
#include "Icons.h"
#include "ImageConvert.h"
#include "compositor/render.h"
#include <QAbstractItemModel>
#include <QApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

namespace {

constexpr int thumbSize = 36;
constexpr int rowRole = Qt::UserRole, idRole = Qt::UserRole + 1;

QIcon channelEye(bool visible, double dpr, QColor color) {
    QPixmap pixmap(int(16 * dpr), int(16 * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    if (!visible) {
        color.setAlpha(60);
        p.setPen(QPen(color, 1));
        p.drawRoundedRect(QRectF(2.5, 2.5, 11, 11), 2, 2);
        return QIcon(pixmap);
    }
    p.setPen(QPen(color, 1.5));
    QPainterPath eye;
    eye.moveTo(1.5, 8);
    eye.quadTo(8, 1.5, 14.5, 8);
    eye.quadTo(8, 14.5, 1.5, 8);
    p.drawPath(eye);
    p.setBrush(color);
    p.drawEllipse(QPointF(8, 8), 2.6, 2.6);
    return QIcon(pixmap);
}

/// A thumbnail framed as the Layers panel frames its own.
QPixmap framed(const QImage& image, double dpr) {
    QPixmap pixmap(int(thumbSize * dpr), int(thumbSize * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(QColor(60, 60, 60));
    QPainter p(&pixmap);
    if (!image.isNull()) {
        const double scale = std::min(thumbSize / double(image.width()), thumbSize / double(image.height()));
        const QSizeF size(image.width() * scale, image.height() * scale);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF((thumbSize - size.width()) / 2, (thumbSize - size.height()) / 2, size.width(), size.height()), image);
    }
    p.setPen(QColor(0, 0, 0, 80));
    p.drawRect(QRectF(0.5, 0.5, thumbSize - 1, thumbSize - 1));
    return pixmap;
}

QString shortcutText(int n) { return QKeySequence(QStringLiteral("Ctrl+%1").arg(n)).toString(QKeySequence::NativeText); }

} // namespace

ChannelsPanel::ChannelsPanel(EditorSession* session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(4, 4, 4, 4);
    list_ = new QListWidget(this);
    list_->setObjectName("channelsList");
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setDefaultDropAction(Qt::MoveAction);
    box->addWidget(list_, 1);
    auto* footer = new QHBoxLayout;
    footer->setSpacing(2);
    auto button = [&](const QString& icon, const QString& tip, auto slot) {
        auto* b = new QToolButton(this);
        b->setIcon(toolIcon(icon, 18));
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, slot);
        footer->addWidget(b);
        return b;
    };
    footer->addStretch();
    button("square-dashed", tr("Load channel as selection"), [this] {
        auto* item = list_->currentItem();
        if (item) loadAsSelection(item->data(rowRole).toInt(), item->data(idRole).toString(), Qt::NoModifier);
    });
    button("mask", tr("Save selection as channel"), [this] {
        if (!session_) return;
        // On the command path (CONTRIBUTING.md, "Commands"), so Actions record it.
        session_->runCommandOr(QStringLiteral("channels.saveSelection"), {}, [this] {
            QString error;
            if (!session_->saveSelectionToChannel(std::nullopt, QString(), SelectionMode::Replace, &error) && !error.isEmpty())
                QMessageBox::information(this, tr("Save Selection"), error);
        });
    });
    button("square-plus", tr("Create new channel"), [this] { if (session_) session_->runCommandOr(QStringLiteral("channels.new"), {}, [this] { session_->newChannel(); }); });
    button("trash-2", tr("Delete current channel"), [this] { if (session_ && session_->targetChannel()) session_->deleteChannel(*session_->targetChannel()); });
    box->addLayout(footer);

    thumbnails_ = new QTimer(this);
    thumbnails_->setSingleShot(true);
    thumbnails_->setInterval(150);
    connect(thumbnails_, &QTimer::timeout, this, &ChannelsPanel::refreshThumbnails);
    connect(list_, &QListWidget::itemClicked, this, &ChannelsPanel::clicked);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (item->data(rowRole).toInt() == Alpha) showOptions(item->data(idRole).toString());
    });
    connect(list_, &QListWidget::customContextMenuRequested, this, &ChannelsPanel::showMenu);
    connect(list_->model(), &QAbstractItemModel::rowsMoved, this, [this] { if (!rebuilding_) QTimer::singleShot(0, this, &ChannelsPanel::rowsMoved); });
    connect(session_, &EditorSession::channelsChanged, this, &ChannelsPanel::rebuild);
    connect(session_, &EditorSession::layersChanged, this, &ChannelsPanel::rebuild);
    connect(session_, &EditorSession::historyChanged, this, &ChannelsPanel::rebuild);
    connect(session_, &EditorSession::documentChanged, this, [this] { thumbnails_->start(); });
    connect(session_, &EditorSession::documentChangedAsShown, this, [this] { thumbnails_->start(); });
    rebuild();
}

QWidget* ChannelsPanel::makeRow(int row, const QString& id, const QString& name, const QString& shortcut, bool visible, bool italic) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(2, 2, 6, 2);
    h->setSpacing(6);
    auto* eye = new QToolButton(w);
    eye->setAutoRaise(true);
    eye->setIcon(channelEye(visible, devicePixelRatioF(), palette().color(QPalette::Text)));
    eye->setIconSize(QSize(16, 16));
    eye->setFixedWidth(22);
    eye->setToolTip(tr("Show or hide the channel"));
    eye->setEnabled(row != QuickMask);
    connect(eye, &QToolButton::clicked, this, [this, row, id, visible] {
        if (!session_) return;
        if (row == Alpha) session_->setAlphaChannelVisible(id.toStdString(), !visible);
        else if (row != QuickMask) session_->setColorChannelVisible(colorBits(row), !visible);
    });
    h->addWidget(eye);
    auto* thumb = new QLabel(w);
    thumb->setObjectName("thumbnail");
    thumb->setFixedSize(thumbSize, thumbSize);
    thumb->setProperty("row", row);
    thumb->setProperty("channelId", id);
    thumb->setToolTip(row == QuickMask ? QString() : tr("Ctrl-click: load as a selection (Shift adds, Alt subtracts, both intersect)"));
    thumb->installEventFilter(this);
    if (auto cached = thumbCache_.find(QString::number(row) + id); cached != thumbCache_.end()) thumb->setPixmap(cached->second);
    h->addWidget(thumb);
    auto* label = new QLabel(name, w);
    if (italic) { QFont f = label->font(); f.setItalic(true); label->setFont(f); }
    h->addWidget(label, 1);
    if (!shortcut.isEmpty()) {
        auto* keys = new QLabel(shortcut, w);
        keys->setEnabled(false);
        h->addWidget(keys);
    }
    return w;
}

void ChannelsPanel::rebuild() {
    if (!session_) return;
    rebuilding_ = true;
    list_->clear();
    const auto& doc = session_->document();
    if (doc) {
        const unsigned visible = session_->visibleColorChannels(), active = session_->activeColorChannels();
        const auto target = session_->targetChannel();
        const bool quickMask = session_->quickMaskLayerId() && session_->activeLayerId() == session_->quickMaskLayerId();
        const bool colorTarget = !target && !quickMask && !session_->isChannelProxy(session_->activeLayerId().value_or(Uuid()));
        auto add = [&](int row, const QString& id, const QString& name, const QString& shortcut, bool shown, bool selected, bool italic = false) {
            auto* item = new QListWidgetItem(list_);
            item->setData(rowRole, row);
            item->setData(idRole, id);
            item->setSizeHint(QSize(0, thumbSize + 6));
            Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
            if (row == Alpha) flags |= Qt::ItemIsDragEnabled;
            item->setFlags(flags);
            list_->setItemWidget(item, makeRow(row, id, name, shortcut, shown, italic));
            item->setSelected(selected);
            if (selected && !list_->currentItem()) list_->setCurrentItem(item, QItemSelectionModel::NoUpdate);
        };
        // The colour channels of the document's mode, as Photoshop names them (Ctrl+2 the composite, then one each).
        const ColorMode mode = doc->colorMode;
        const unsigned all = session_->allColors();
        const int colours = colorModeColorChannels(mode);
        const QString composite = mode == ColorMode::CMYK ? tr("CMYK") : mode == ColorMode::Lab ? tr("Lab") : tr("RGB");
        const QString names[3][4] = {{tr("Red"), tr("Green"), tr("Blue"), QString()},
                                     {tr("Cyan"), tr("Magenta"), tr("Yellow"), tr("Black")},
                                     {tr("Lightness"), tr("a"), tr("b"), QString()}};
        const int set = mode == ColorMode::CMYK ? 1 : mode == ColorMode::Lab ? 2 : 0;
        add(Composite, {}, composite, shortcutText(2), visible == all, colorTarget && active == all);
        for (int c = 0; c < colours; c++) add(c < 3 ? Red + c : Fourth, {}, names[set][c], shortcutText(3 + c), visible >> c & 1, colorTarget && (active >> c & 1));
        int n = 0;
        for (const Channel& c : doc->channels) {
            const QString id = QString::fromStdString(c.id);
            add(Alpha, id, QString::fromStdString(c.name), n < 4 ? shortcutText(3 + colours + n) : QString(), session_->visibleAlphaChannels().count(c.id) > 0, target == c.id);
            n++;
        }
        // Quick Mask, while it is on: the selection being painted, as Photoshop lists it.
        if (session_->quickMaskActive()) add(QuickMask, {}, tr("Quick Mask"), QString(), true, quickMask, true);
    }
    rebuilding_ = false;
    // The thumbnails as last drawn, then drawn again once the edits pause (a render of the composite).
    if (thumbCache_.empty()) refreshThumbnails(); else thumbnails_->start();
}

void ChannelsPanel::refreshThumbnails() {
    if (!session_ || !session_->document()) return;
    const Document& doc = *session_->document();
    const double dpr = devicePixelRatioF();
    // The composite, small, without the layers that only stand in for a channel being painted.
    Document shown = doc;
    shown.layers.erase(std::remove_if(shown.layers.begin(), shown.layers.end(), [&](const Layer& l) { return session_->isChannelProxy(l.id) || session_->quickMaskLayerId() == l.id; }), shown.layers.end());
    const double scale = std::min(1.0, double(thumbSize * 2) / std::max(doc.width, doc.height));
    RenderOptions options;
    options.region = doc.rect();
    options.scale = scale;
    Image composite;
    compositor::render(shown, options, composite);
    // CMYK and Lab: the colour channels come from the composite at the document's layout (a plate's ink dark over
    // white paper; L, a and b as values, a and b gray where neutral).
    const AnyImage native = doc.colorMode == ColorMode::RGB ? AnyImage() : compositor::renderNative(shown, options);
    auto modeChannelImage = [&](int c) {
        QImage g(native.width(), native.height(), QImage::Format_Grayscale8);
        const int n = native.channels(), alpha = n - 1;
        for (int y = 0; y < native.height(); y++) {
            uchar* d = g.scanLine(y);
            for (int x = 0; x < native.width(); x++) {
                double v, a;
                if (native.u16()) { const uint16_t* p = native.u16()->pixel(x, y); v = p[c] / 32768.0; a = p[alpha] / 32768.0; }
                else if (native.c8()) { const uint8_t* p = native.c8()->pixel(x, y); v = p[c] / 255.0; a = p[alpha] / 255.0; }
                else { const uint8_t* p = native.u8()->pixel(x, y); v = p[c] / 255.0; a = p[alpha] / 255.0; }
                const double paper = doc.colorMode == ColorMode::Lab && c > 0 ? 0.5 : 1.0;
                d[x] = uchar(std::lround(std::clamp(v + paper * (1 - a), 0.0, 1.0) * 255));
            }
        }
        return g;
    };
    // Over white, as Photoshop's channel thumbnails show transparency.
    QImage rgb(composite.width(), composite.height(), QImage::Format_RGB32);
    for (int y = 0; y < composite.height(); y++) {
        const uint8_t* s = composite.row(y);
        QRgb* d = reinterpret_cast<QRgb*>(rgb.scanLine(y));
        for (int x = 0; x < composite.width(); x++, s += 4) {
            const int white = 255 - s[3];
            d[x] = qRgb(std::min(255, s[0] + white), std::min(255, s[1] + white), std::min(255, s[2] + white));
        }
    }
    auto channelImage = [&](int c) {
        QImage g(rgb.width(), rgb.height(), QImage::Format_Grayscale8);
        for (int y = 0; y < rgb.height(); y++) {
            const QRgb* s = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
            uchar* d = g.scanLine(y);
            for (int x = 0; x < rgb.width(); x++) d[x] = uchar(c == 0 ? qRed(s[x]) : c == 1 ? qGreen(s[x]) : qBlue(s[x]));
        }
        return g;
    };
    for (int i = 0; i < list_->count(); i++) {
        QListWidgetItem* item = list_->item(i);
        QWidget* row = list_->itemWidget(item);
        auto* thumb = row ? row->findChild<QLabel*>("thumbnail") : nullptr;
        if (!thumb) continue;
        const int kind = item->data(rowRole).toInt();
        QImage image;
        if (kind == Composite) image = rgb;
        else if ((kind >= Red && kind <= Blue) || kind == Fourth) {
            const int c = kind == Fourth ? 3 : kind - Red;
            image = native ? modeChannelImage(c) : channelImage(c);
        }
        else if (kind == Alpha) {
            if (const Channel* c = findChannel(doc, item->data(idRole).toString().toStdString()))
                if (auto small = channelThumbnail(c->image, thumbSize * 2)) image = toQImage(*small);
        } else if (kind == QuickMask) {
            // The Quick Mask's layer mask holds the inverse of the selection: shown as the selection.
            const Layer* layer = session_->quickMaskLayerId() ? doc.find(*session_->quickMaskLayerId()) : nullptr;
            if (layer && layer->mask && layer->mask->asset.thumbnail) { image = toQImage(*layer->mask->asset.thumbnail); image.invertPixels(); }
        }
        const QPixmap pixmap = framed(image, dpr);
        thumb->setPixmap(pixmap);
        thumbCache_[QString::number(kind) + item->data(idRole).toString()] = pixmap;
    }
}

bool ChannelsPanel::eventFilter(QObject* watched, QEvent* event) {
    // Ctrl-click on a thumbnail loads the channel as a selection; any other click selects the row as usual.
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        auto* thumb = qobject_cast<QLabel*>(watched);
        if (thumb && mouse->button() == Qt::LeftButton && (mouse->modifiers() & Qt::ControlModifier)) {
            loadAsSelection(thumb->property("row").toInt(), thumb->property("channelId").toString(), mouse->modifiers());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ChannelsPanel::loadAsSelection(int row, const QString& id, Qt::KeyboardModifiers modifiers) {
    if (!session_ || row == QuickMask) return;
    SelectionSource source;
    source.kind = row == Composite ? SelectionSource::Composite : row == Red ? SelectionSource::Red : row == Green ? SelectionSource::Green
                  : row == Blue ? SelectionSource::Blue : row == Fourth ? SelectionSource::Black : SelectionSource::AlphaChannel;
    source.id = id.toStdString();
    QString error;
    const SelectionMode mode = thumbnailClickMode(modifiers & Qt::ShiftModifier, modifiers & Qt::AltModifier);
    if (!session_->loadSelectionFromSource(source, false, mode, &error) && !error.isEmpty()) QMessageBox::information(this, tr("Load Selection"), error);
}

void ChannelsPanel::clicked(QListWidgetItem* item) {
    if (!session_ || !item) return;
    const int row = item->data(rowRole).toInt();
    const bool shift = QApplication::keyboardModifiers() & Qt::ShiftModifier;
    if (row == Alpha) session_->selectAlphaChannel(item->data(idRole).toString().toStdString(), shift);
    else if (row == QuickMask) { if (auto id = session_->quickMaskLayerId()) session_->selectLayer(*id, true); }
    else session_->selectColorChannels(colorBits(row), shift);
    rebuild();
}

unsigned ChannelsPanel::colorBits(int row) const {
    if (row == Composite) return session_ ? session_->allColors() : colorChannelsAll;
    if (row == Fourth) return 8u;
    return 1u << (row - Red);
}

void ChannelsPanel::showOptions(const QString& id) {
    if (!session_) return;
    (new ChannelOptionsDialog(session_, id.toStdString(), this))->open();
}

void ChannelsPanel::showMenu(const QPoint& at) {
    if (!session_ || !session_->document()) return;
    QListWidgetItem* item = list_->itemAt(at);
    const int row = item ? item->data(rowRole).toInt() : -1;
    const QString id = item ? item->data(idRole).toString() : QString();
    QMenu menu(this);
    menu.addAction(tr("New Channel…"), this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Channel"), tr("Name:"), QLineEdit::Normal,
                                                   QString::fromStdString(nextChannelName(*session_->document(), QCoreApplication::translate("Names", "Alpha").toStdString())), &ok);
        if (ok) session_->runCommandOr(QStringLiteral("channels.new"), {{"name", name}}, [this, name] { session_->newChannel(name); });
    });
    if (row == Alpha) {
        menu.addAction(tr("Duplicate Channel…"), this, [this, id] {
            const Channel* c = findChannel(*session_->document(), id.toStdString());
            if (!c) return;
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Duplicate Channel"), tr("As:"), QLineEdit::Normal, tr("%1 copy").arg(QString::fromStdString(c->name)), &ok);
            if (ok) session_->duplicateChannel(id.toStdString(), name);
        });
        menu.addAction(tr("Delete Channel"), this, [this, id] { session_->deleteChannel(id.toStdString()); });
        menu.addAction(tr("Rename…"), this, [this, id] {
            const Channel* c = findChannel(*session_->document(), id.toStdString());
            if (!c) return;
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename Channel"), tr("Name:"), QLineEdit::Normal, QString::fromStdString(c->name), &ok);
            if (ok) session_->renameChannel(id.toStdString(), name);
        });
        menu.addAction(tr("Channel Options…"), this, [this, id] { showOptions(id); });
    }
    if (row >= Composite && row <= Alpha) {
        menu.addSeparator();
        menu.addAction(tr("Load as Selection"), this, [this, row, id] { loadAsSelection(row, id, Qt::NoModifier); });
    }
    menu.exec(list_->viewport()->mapToGlobal(at));
}

void ChannelsPanel::rowsMoved() {
    // A dragged alpha channel: its place among the alpha channels is where it was dropped.
    if (!session_ || !session_->document()) return;
    std::vector<Uuid> order;
    for (int i = 0; i < list_->count(); i++)
        if (list_->item(i)->data(rowRole).toInt() == Alpha) order.push_back(list_->item(i)->data(idRole).toString().toStdString());
    QListWidgetItem* dragged = list_->currentItem();
    if (dragged && dragged->data(rowRole).toInt() == Alpha) {
        const Uuid id = dragged->data(idRole).toString().toStdString();
        const auto at = std::find(order.begin(), order.end(), id);
        if (at != order.end()) session_->moveChannel(id, int(at - order.begin()));
    }
    rebuild();
}

} // namespace app
