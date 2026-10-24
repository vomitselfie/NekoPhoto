// The edges' own pages: see EdgePageWidgets.h.
#include "EdgePageWidgets.h"
#include "ColorSwatches.h"
#include "Style.h"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace app {

// ---- HsvPicker ------------------------------------------------------------------------------------------------

HsvPicker::HsvPicker(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("hsvPicker"));
    setCursor(Qt::CrossCursor);
    setToolTip(tr("Pick the foreground colour: saturation across, brightness up, the hue on the right"));
}

QRect HsvPicker::hueRect() const { return QRect(width() - 16, 0, 16, height()); }
QRect HsvPicker::squareRect() const { return QRect(0, 0, std::max(1, width() - 24), height()); }

void HsvPicker::setColor(const QColor& c) {
    if (dragging_ != None || !c.isValid()) return;   // the person's drag wins over the colour it set
    const QColor hsv = c.toHsv();
    float h = 0, s = 0, v = 0;
    hsv.getHsvF(&h, &s, &v);
    if (h >= 0) h_ = h;   // a grey keeps the hue it had
    s_ = s;
    v_ = v;
    update();
}

void HsvPicker::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect sq = squareRect(), hue = hueRect();
    const qreal dpr = devicePixelRatioF();
    const QSize px(int(sq.width() * dpr), int(sq.height() * dpr));
    if (square_.size() != px || squareHue_ != h_) {
        // The square for this hue, drawn once per hue and size.
        square_ = QImage(px, QImage::Format_RGB32);
        for (int y = 0; y < px.height(); y++) {
            auto* line = reinterpret_cast<QRgb*>(square_.scanLine(y));
            const double v = 1.0 - double(y) / std::max(1, px.height() - 1);
            for (int x = 0; x < px.width(); x++) line[x] = QColor::fromHsvF(float(h_), float(double(x) / std::max(1, px.width() - 1)), float(v)).rgb();
        }
        square_.setDevicePixelRatio(dpr);
        squareHue_ = h_;
    }
    QPainterPath clip;
    clip.addRoundedRect(QRectF(sq), 6, 6);
    p.save();
    p.setClipPath(clip);
    p.drawImage(sq.topLeft(), square_);
    p.restore();
    QLinearGradient bar(hue.topLeft(), hue.bottomLeft());
    for (int i = 0; i <= 6; i++) bar.setColorAt(i / 6.0, QColor::fromHsvF(float(i / 6.0 >= 1 ? 0.0 : i / 6.0), 1, 1));
    QPainterPath hueShape;
    hueShape.addRoundedRect(QRectF(hue), 6, 6);
    p.fillPath(hueShape, bar);
    // The markers: a ring on the square, a notch on the bar.
    const QPointF at(sq.left() + s_ * (sq.width() - 1), sq.top() + (1 - v_) * (sq.height() - 1));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 3));
    p.drawEllipse(at, 5, 5);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawEllipse(at, 5, 5);
    const double hy = hue.top() + h_ * (hue.height() - 1);
    p.setPen(QPen(Qt::black, 3));
    p.drawRoundedRect(QRectF(hue.left() - 1, hy - 3, hue.width() + 2, 6), 3, 3);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawRoundedRect(QRectF(hue.left() - 1, hy - 3, hue.width() + 2, 6), 3, 3);
}

void HsvPicker::pick(QPoint p) {
    if (dragging_ == Hue) {
        const QRect r = hueRect();
        h_ = std::clamp(double(p.y() - r.top()) / std::max(1, r.height() - 1), 0.0, 0.9999);
    } else {
        const QRect r = squareRect();
        s_ = std::clamp(double(p.x() - r.left()) / std::max(1, r.width() - 1), 0.0, 1.0);
        v_ = 1 - std::clamp(double(p.y() - r.top()) / std::max(1, r.height() - 1), 0.0, 1.0);
    }
    update();
    emit colorChanging(color());
}

void HsvPicker::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = hueRect().adjusted(-4, 0, 0, 0).contains(e->position().toPoint()) ? Hue : Square;
    pick(e->position().toPoint());
}

void HsvPicker::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_ != None) pick(e->position().toPoint());
}

void HsvPicker::mouseReleaseEvent(QMouseEvent* e) {
    if (dragging_ == None || e->button() != Qt::LeftButton) return;
    pick(e->position().toPoint());
    dragging_ = None;
    emit colorPicked(color());
}

// ---- SwatchGrid -----------------------------------------------------------------------------------------------

SwatchGrid::SwatchGrid(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setCursor(Qt::PointingHandCursor);
}

void SwatchGrid::setColors(const QList<QColor>& colors) {
    colors_ = colors;
    updateGeometry();
    update();
}

int SwatchGrid::heightForWidth(int width) const {
    const int rows = colors_.isEmpty() ? 1 : (int(colors_.size()) + columns - 1) / columns;
    return rows * (cell(width) + gap) - gap;
}

QSize SwatchGrid::sizeHint() const { return {columns * (18 + gap), heightForWidth(columns * (18 + gap))}; }

void SwatchGrid::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int side = cell(width());
    QColor edge = palette().color(QPalette::WindowText);
    edge.setAlpha(70);
    for (int i = 0; i < colors_.size(); i++) {
        const QRectF r((i % columns) * (side + gap) + 0.5, (i / columns) * (side + gap) + 0.5, side - 1, side - 1);
        QPainterPath shape;
        shape.addRoundedRect(r, 4, 4);
        p.fillPath(shape, colors_[i]);
        p.setPen(QPen(edge, 1));
        p.drawPath(shape);
    }
}

void SwatchGrid::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const int side = cell(width());
    const int col = int(e->position().x()) / (side + gap), row = int(e->position().y()) / (side + gap);
    const int i = row * columns + col;
    if (col < columns && i >= 0 && i < colors_.size()) emit chosen(colors_[i], e->modifiers() & Qt::ControlModifier);
}

// ---- ColourPage -----------------------------------------------------------------------------------------------

QList<QColor> ColourPage::defaultSwatches() {
    QList<QColor> out;
    // Pure, light and dark tones of the primaries and secondaries with orange and violet, then a row of greys: ten to a
    // row.
    const int hues[] = {0, 30, 60, 120, 180, 210, 240, 280, 300, 330};
    for (int tone = 0; tone < 3; tone++)
        for (int h : hues) out << (tone == 0 ? QColor::fromHsv(h, 255, 255) : tone == 1 ? QColor::fromHsv(h, 110, 255) : QColor::fromHsv(h, 255, 140));
    for (int i = 0; i < 10; i++) out << QColor::fromHsv(0, 0, int(std::lround(255.0 * i / 9)));
    return out;
}

ColourPage::ColourPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("colourPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 6);
    layout->setSpacing(6);
    auto* top = new QHBoxLayout;
    squares_ = new ColorSwatches;
    top->addWidget(squares_, 0, Qt::AlignTop);
    picker_ = new HsvPicker;
    top->addWidget(picker_, 1);
    layout->addLayout(top);
    auto* more = new QPushButton(tr("Color Picker…"));
    more->setToolTip(tr("Choose the foreground colour in the Color Picker"));
    connect(more, &QPushButton::clicked, this, [this] { emit dialogRequested(false); });
    layout->addWidget(more);
    auto* swatchLabel = new QLabel(tr("Swatches"));
    swatchLabel->setStyleSheet(hintStyle());
    layout->addWidget(swatchLabel);
    swatches_ = new SwatchGrid;
    swatches_->setObjectName(QStringLiteral("swatchGrid"));
    swatches_->setToolTip(tr("Click for the foreground colour, Ctrl+click for the background"));
    swatches_->setColors(defaultSwatches());
    layout->addWidget(swatches_);
    recentLabel_ = new QLabel(tr("Recent colours"));
    recentLabel_->setStyleSheet(hintStyle());
    layout->addWidget(recentLabel_);
    recent_ = new SwatchGrid;
    recent_->setObjectName(QStringLiteral("recentColours"));
    recent_->setToolTip(tr("The foreground colours used last"));
    layout->addWidget(recent_);
    layout->addStretch(1);
    connect(squares_, &ColorSwatches::foregroundClicked, this, [this] { emit dialogRequested(false); });
    connect(squares_, &ColorSwatches::backgroundClicked, this, [this] { emit dialogRequested(true); });
    connect(squares_, &ColorSwatches::swapRequested, this, &ColourPage::swapRequested);
    connect(squares_, &ColorSwatches::defaultsRequested, this, &ColourPage::defaultsRequested);
    // The square shows the colour as it is dragged; it is set when the button comes up.
    connect(picker_, &HsvPicker::colorChanging, this, [this](const QColor& c) { squares_->setColors(c, squares_->background()); });
    connect(picker_, &HsvPicker::colorPicked, this, &ColourPage::foregroundPicked);
    for (SwatchGrid* grid : {swatches_, recent_})
        connect(grid, &SwatchGrid::chosen, this, [this](const QColor& c, bool background) { if (background) emit backgroundPicked(c); else emit foregroundPicked(c); });
    setRecent({});
}

void ColourPage::setColors(const QColor& foreground, const QColor& background) {
    squares_->setColors(foreground, background);
    picker_->setColor(foreground);
}

void ColourPage::setRecent(const QList<QColor>& recent) {
    recent_->setColors(recent);
    recent_->setVisible(!recent.isEmpty());
    recentLabel_->setVisible(!recent.isEmpty());
}

// ---- DocumentsPage --------------------------------------------------------------------------------------------

DocumentsPage::DocumentsPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("documentsPage"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 2);
    layout->setSpacing(8);
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    auto* strip = new QWidget;
    strip->setObjectName(QStringLiteral("documentTiles"));
    row_ = new QHBoxLayout(strip);
    row_->setContentsMargins(0, 0, 0, 0);
    row_->setSpacing(6);
    row_->addStretch(1);
    scroll->setWidget(strip);
    scroll->setMinimumHeight(84);
    layout->addWidget(scroll, 1);
    auto* side = new QVBoxLayout;
    side->setSpacing(4);
    zoom_ = new QComboBox;
    zoom_->setObjectName(QStringLiteral("documentsZoom"));
    zoom_->setToolTip(tr("Zoom of the current document"));
    for (int z : {25, 50, 100, 200, 400, 800}) zoom_->addItem(QStringLiteral("%1%").arg(z), z / 100.0);
    zoom_->addItem(tr("Fit"), 0.0);
    connect(zoom_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) { emit zoomChosen(zoom_->itemData(i).toDouble()); });
    side->addWidget(zoom_);
    description_ = new QLabel;
    description_->setObjectName(QStringLiteral("documentDescription"));
    description_->setStyleSheet(hintStyle());
    side->addWidget(description_);
    side->addStretch(1);
    layout->addLayout(side);
}

void DocumentsPage::setDocuments(const std::vector<Document>& documents) {
    for (QWidget* t : tiles_) { row_->removeWidget(t); t->deleteLater(); }
    tiles_.clear();
    for (size_t i = 0; i < documents.size(); i++) {
        const Document& d = documents[i];
        auto* tile = new QToolButton;
        tile->setObjectName(QStringLiteral("documentTile"));
        tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tile->setCheckable(true);
        tile->setChecked(d.current);
        tile->setAutoRaise(true);
        tile->setIconSize(QSize(64, 48));
        QPixmap icon(QSize(64, 48) * devicePixelRatioF());
        icon.setDevicePixelRatio(devicePixelRatioF());
        icon.fill(Qt::transparent);
        {
            QPainter p(&icon);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const QRectF box(0.5, 0.5, 63, 47);
            QPainterPath shape;
            shape.addRoundedRect(box, 6, 6);
            QColor back = palette().color(QPalette::Base);
            p.fillPath(shape, back);
            if (!d.thumbnail.isNull()) {
                const QSizeF fit = QSizeF(d.thumbnail.size()).scaled(QSizeF(60, 44), Qt::KeepAspectRatio);
                const QRectF at(QPointF(32 - fit.width() / 2, 24 - fit.height() / 2), fit);
                // A checkerboard under transparent pixels.
                p.save();
                p.setClipRect(at);
                for (int y = 0; y < 48; y += 6)
                    for (int x = 0; x < 64; x += 6) p.fillRect(QRect(x, y, 6, 6), ((x + y) / 6) % 2 ? QColor(204, 204, 204) : QColor(255, 255, 255));
                p.restore();
                p.drawImage(at, d.thumbnail);
            }
            p.setPen(QPen(d.current ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid), d.current ? 2 : 1));
            p.drawPath(shape);
        }
        tile->setIcon(QIcon(icon));
        tile->setText(QFontMetrics(tile->font()).elidedText(d.title, Qt::ElideMiddle, 96));
        tile->setToolTip(d.title);
        const int index = int(i);
        connect(tile, &QToolButton::clicked, this, [this, index] { emit documentChosen(index); });
        row_->insertWidget(int(i), tile);
        tiles_.push_back(tile);
    }
}

void DocumentsPage::setCurrent(double zoom, const QString& description) {
    const QString text = QStringLiteral("%1%").arg(std::lround(zoom * 100));
    int at = -1;
    for (int i = 0; i < zoom_->count(); i++) if (zoom_->itemText(i) == text) at = i;
    if (at < 0) {
        // Not one of the steps: shown as the field's text over the list.
        zoom_->setEditable(true);
        zoom_->lineEdit()->setReadOnly(true);
        zoom_->setEditText(text);
    } else {
        zoom_->setEditable(false);
        zoom_->setCurrentIndex(at);
    }
    description_->setText(description);
}

// ---- InfoPanel ------------------------------------------------------------------------------------------------

InfoPanel::InfoPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("infoPanel"));
    auto* form = new QFormLayout(this);
    form->setContentsMargins(8, 4, 8, 4);
    form->setVerticalSpacing(3);
    pointer_ = new QLabel(QStringLiteral("–"));
    pointer_->setObjectName(QStringLiteral("infoPointer"));
    size_ = new QLabel;
    mode_ = new QLabel;
    zoom_ = new QLabel;
    for (QLabel* l : {pointer_, size_, mode_, zoom_}) l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Pointer"), pointer_);
    form->addRow(tr("Size"), size_);
    form->addRow(tr("Mode"), mode_);
    form->addRow(tr("Zoom"), zoom_);
    setMinimumWidth(170);
}

void InfoPanel::setPointer(QPointF p) {
    pointer_->setText(tr("X %1   Y %2").arg(int(std::floor(p.x()))).arg(int(std::floor(p.y()))));
}

void InfoPanel::setDocument(const QString& size, const QString& mode, const QString& zoom) {
    size_->setText(size);
    mode_->setText(mode);
    zoom_->setText(zoom);
}

} // namespace app
