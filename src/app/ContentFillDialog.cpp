#include "ContentFillDialog.h"
#include "ActionLibrary.h"
#include "ImageConvert.h"
#include "compositor/render.h"
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

using namespace compositor;

namespace app {

SamplingCanvas::SamplingCanvas(const QImage& picture, const QImage& selection, QWidget* parent)
    : QWidget(parent), picture_(picture), selection_(selection) {
    // The custom area starts as everything but the selection, as Photoshop's does.
    area_ = QImage(picture.size(), QImage::Format_Grayscale8);
    area_.fill(255);
    for (int y = 0; y < area_.height(); y++) {
        uchar* a = area_.scanLine(y);
        const uchar* s = selection_.constScanLine(y);
        for (int x = 0; x < area_.width(); x++) if (s[x] >= 128) a[x] = 0;
    }
    setFixedSize(picture.size());
    setCursor(Qt::CrossCursor);
}

void SamplingCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.drawImage(0, 0, picture_);
    // Green over where the fill copies from, red over the selection.
    const QImage& area = custom_ ? area_ : autoArea_;
    QImage tint(picture_.size(), QImage::Format_ARGB32_Premultiplied);
    tint.fill(Qt::transparent);
    for (int y = 0; y < tint.height(); y++) {
        QRgb* t = reinterpret_cast<QRgb*>(tint.scanLine(y));
        const uchar* s = selection_.constScanLine(y);
        const uchar* a = area.isNull() ? nullptr : area.constScanLine(y);
        for (int x = 0; x < tint.width(); x++) {
            if (s[x] >= 128) t[x] = qPremultiply(qRgba(220, 40, 40, 110));
            else if (all_ || (a && a[x] >= 128)) t[x] = qPremultiply(qRgba(40, 200, 80, 90));
        }
    }
    p.drawImage(0, 0, tint);
}

void SamplingCanvas::dab(QPointF at, bool add) {
    if (!custom_) return;
    QPainter p(&area_);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    p.setBrush(add ? Qt::white : Qt::black);
    p.drawEllipse(at, brush_ / 2.0, brush_ / 2.0);
    update();
}

void SamplingCanvas::mousePressEvent(QMouseEvent* event) {
    dab(event->position(), event->button() == Qt::LeftButton && !(event->modifiers() & Qt::AltModifier));
}

void SamplingCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & (Qt::LeftButton | Qt::RightButton))
        dab(event->position(), (event->buttons() & Qt::LeftButton) && !(event->modifiers() & Qt::AltModifier));
}

ContentFillDialog::ContentFillDialog(EditorSession* session, QWidget* parent) : QDialog(parent), session_(session) {
    setWindowTitle(tr("Content-Aware Fill"));
    setAttribute(Qt::WA_DeleteOnClose);
    const Document& doc = *session->document();
    // The picture: the document as shown, scaled to fit.
    auto flat = renderFlattened(doc);
    const double scale = std::min({1.0, 560.0 / doc.width, 420.0 / doc.height});
    const QSize size(std::max(1, int(doc.width * scale + 0.5)), std::max(1, int(doc.height * scale + 0.5)));
    QImage picture = toQImage(*flat).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage selection(size, QImage::Format_Grayscale8);
    selection.fill(0);
    if (doc.selection && doc.selection->coverage) selection = toQImage(*doc.selection->coverage).convertToFormat(QImage::Format_Grayscale8).scaled(size);
    canvas_ = new SamplingCanvas(picture, selection);
    // Auto's area, roughly: the neighbourhood the fill searches (twice the selection's size around it, 64 to 384 pixels).
    if (doc.selection) {
        const Rect b = doc.selection->bounds();
        const double reach = std::clamp(2 * std::max(b.width, b.height), 64.0, 384.0);
        QImage autoArea(size, QImage::Format_Grayscale8);
        autoArea.fill(0);
        QPainter p(&autoArea);
        p.fillRect(QRectF((b.x - reach) * scale, (b.y - reach) * scale, (b.width + 2 * reach) * scale, (b.height + 2 * reach) * scale), Qt::white);
        p.end();
        canvas_->setAutoArea(autoArea);
    }

    auto* layout = new QHBoxLayout(this);
    layout->addWidget(canvas_, 0, Qt::AlignTop);
    auto* side = new QVBoxLayout;
    layout->addLayout(side);

    auto* samplingBox = new QGroupBox(tr("Sampling Area"));
    auto* sl = new QVBoxLayout(samplingBox);
    sampling_ = new QButtonGroup(this);
    const QStringList names{tr("Auto"), tr("All of the Layer"), tr("Custom")};
    const QStringList tips{tr("Copy from around the selection"), tr("Copy from anywhere on the layer"),
                           tr("Copy only from the area painted green: the left button adds, the right button or Alt removes")};
    for (int i = 0; i < names.size(); i++) {
        auto* b = new QRadioButton(names[i]);
        b->setToolTip(tips[i]);
        sampling_->addButton(b, i);
        sl->addWidget(b);
    }
    sampling_->button(0)->setChecked(true);
    auto* brushRow = new QHBoxLayout;
    brushRow->addWidget(new QLabel(tr("Brush")));
    brushSize_ = new QSpinBox;
    brushSize_->setRange(2, 400);
    brushSize_->setValue(24);
    brushSize_->setSuffix(tr(" px"));
    brushSize_->setToolTip(tr("The sampling brush's diameter on this picture"));
    brushSize_->setEnabled(false);
    brushRow->addWidget(brushSize_);
    sl->addLayout(brushRow);
    side->addWidget(samplingBox);
    connect(brushSize_, QOverload<int>::of(&QSpinBox::valueChanged), canvas_, &SamplingCanvas::setBrush);
    connect(sampling_, &QButtonGroup::idClicked, this, [this](int id) {
        canvas_->setCustom(id == 2);
        canvas_->setAll(id == 1);
        brushSize_->setEnabled(id == 2);
    });

    auto* outputBox = new QGroupBox(tr("Output"));
    auto* ol = new QHBoxLayout(outputBox);
    ol->addWidget(new QLabel(tr("Output To")));
    output_ = new QComboBox;
    output_->addItems({tr("Current Layer"), tr("New Layer")});
    ol->addWidget(output_);
    side->addWidget(outputBox);

    auto* preview = new QPushButton(tr("Preview"));
    preview->setToolTip(tr("Show the fill on the canvas"));
    connect(preview, &QPushButton::clicked, this, &ContentFillDialog::previewFill);
    side->addWidget(preview);
    side->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ContentFillDialog::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    side->addWidget(buttons);
}

ContentFillDialog::~ContentFillDialog() {
    if (session_ && previewShown_) session_->clearPixelPreview();
}

ContentFillRequest ContentFillDialog::request() const {
    ContentFillRequest r;
    r.sampling = ContentFillRequest::Sampling(std::clamp(sampling_->checkedId(), 0, 2));
    r.newLayer = output_->currentIndex() == 1;
    if (r.sampling == ContentFillRequest::Sampling::Custom && session_ && session_->document()) {
        const Document& doc = *session_->document();
        r.sampleArea = grayFromQImage(canvas_->area().scaled(doc.width, doc.height));
    }
    return r;
}

void ContentFillDialog::previewFill() {
    if (!session_ || !session_->document()) return;
    ContentFillRequest r = request();
    r.newLayer = false;   // the preview shows the layer as it would look
    LayerTransform placed;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto result = session_->contentAwareFillResult(r, placed, &error);
    QApplication::restoreOverrideCursor();
    if (!result) { QMessageBox::warning(this, windowTitle(), error); return; }
    session_->setPixelPreview(result, placed);
    previewShown_ = true;
}

void ContentFillDialog::apply() {
    if (!session_ || !session_->document()) { reject(); return; }
    if (previewShown_) { session_->clearPixelPreview(); previewShown_ = false; }
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const ContentFillRequest r = request();
    const bool done = session_->contentAwareFill(&error, r);
    QApplication::restoreOverrideCursor();
    if (!done) { QMessageBox::warning(this, windowTitle(), error); return; }
    // A painted custom sampling area has no request form (the method takes rectangles), so only Auto and All record.
    if (r.sampling != ContentFillRequest::Sampling::Custom)
        recordAction("pixels.contentAwareFill", {{"sampling", r.sampling == ContentFillRequest::Sampling::All ? "all" : "auto"}, {"output", r.newLayer ? "new" : "current"}});
    accept();
}

} // namespace app
