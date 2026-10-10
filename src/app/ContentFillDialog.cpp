#include "Theme.h"
#include "ContentFillDialog.h"
#include "ActionLibrary.h"
#include "ImageConvert.h"
#include "compositor/inpaint.h"
#include "compositor/render.h"
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <algorithm>

using namespace compositor;

namespace app {

SamplingCanvas::SamplingCanvas(const QImage& picture, const QImage& selection, QWidget* parent)
    : QWidget(parent), picture_(picture), selection_(selection) {
    setFixedSize(picture.size());
}

void SamplingCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.drawImage(0, 0, picture_);
    // Green over where the fill copies from, red over the selection.
    const QImage& area = autoArea_;
    QImage tint(picture_.size(), QImage::Format_ARGB32_Premultiplied);
    tint.fill(Qt::transparent);
    for (int y = 0; y < tint.height(); y++) {
        QRgb* t = reinterpret_cast<QRgb*>(tint.scanLine(y));
        const uchar* s = selection_.constScanLine(y);
        const uchar* a = area.isNull() ? nullptr : area.constScanLine(y);
        for (int x = 0; x < tint.width(); x++) {
            if (s[x] >= 128) t[x] = qPremultiply(qRgba(220, 40, 40, 110));
            else if (a && a[x] >= 128) t[x] = qPremultiply(qRgba(40, 200, 80, 90));
        }
    }
    p.drawImage(0, 0, tint);
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
    if (doc.selection && doc.selection->coverage.u8()) selection = toQImage(*doc.selection->coverage.u8()).convertToFormat(QImage::Format_Grayscale8).scaled(size);
    canvas_ = new SamplingCanvas(picture, selection);
    // The areas the fill searches: the window scanned around each patch, reaching that far past the selection
    // (compositor/inpaint.h: the search radius plus a patch).
    if (doc.selection) {
        const Rect b = doc.selection->bounds();
        const InpaintOptions defaults;
        auto box = [&](double reach) {
            QImage area(size, QImage::Format_Grayscale8);
            area.fill(0);
            QPainter p(&area);
            p.fillRect(QRectF((b.x - reach) * scale, (b.y - reach) * scale, (b.width + 2 * reach) * scale, (b.height + 2 * reach) * scale), Qt::white);
            p.end();
            return area;
        };
        autoArea_ = box(defaults.searchRadius + 2 * defaults.patchRadius + 1);
        wideArea_ = box(defaults.wideSearchRadius + 2 * defaults.patchRadius + 1);
        canvas_->setAutoArea(autoArea_);
    }

    auto* layout = new QHBoxLayout(this);
    layout->addWidget(canvas_, 0, Qt::AlignTop);
    auto* side = new QVBoxLayout;
    layout->addLayout(side);

    auto* samplingBox = new QGroupBox(tr("Sampling Area"));
    auto* sl = new QVBoxLayout(samplingBox);
    sampling_ = new QButtonGroup(this);
    // A painted (custom) sampling area is not offered: see docs/legal-boundaries.md, "Content-Aware Fill".
    const QStringList names{tr("Auto"), tr("Wide Area")};
    const QStringList tips{tr("Copy from around the selection"), tr("Copy from a wider area around the selection (slower)")};
    for (int i = 0; i < names.size(); i++) {
        auto* b = new QRadioButton(names[i]);
        b->setToolTip(tips[i]);
        sampling_->addButton(b, i);
        sl->addWidget(b);
    }
    sampling_->button(0)->setChecked(true);
    auto* note = new QLabel(tr("A hand-painted sampling area is not available in NekoPhoto."));
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("contentFillCustomNote"));
    sl->addWidget(note);
    side->addWidget(samplingBox);
    connect(sampling_, &QButtonGroup::idClicked, this, [this](int id) { canvas_->setAutoArea(id == 1 ? wideArea_ : autoArea_); });

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
    r.sampling = sampling_->checkedId() == 1 ? ContentFillRequest::Sampling::All : ContentFillRequest::Sampling::Auto;
    r.newLayer = output_->currentIndex() == 1;
    return r;
}

void ContentFillDialog::previewFill() {
    if (!session_ || !session_->document()) return;
    ContentFillRequest r = request();
    r.newLayer = false;   // the preview shows the layer as it would look
    LayerTransform placed;
    QString error;
    QApplication::setOverrideCursor(themedCursor(Qt::WaitCursor));
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
    const ContentFillRequest r = request();
    // Both choices are pixels.contentAwareFill (the command path; an error is shown by it).
    const QJsonObject step{{"sampling", r.sampling == ContentFillRequest::Sampling::All ? "all" : "auto"}, {"output", r.newLayer ? "new" : "current"}};
    if (session_->commandsRouted()) {
        QApplication::setOverrideCursor(themedCursor(Qt::WaitCursor));
        const bool filled = session_->runCommand(QStringLiteral("pixels.contentAwareFill"), step).has_value();
        QApplication::restoreOverrideCursor();
        if (filled) accept();
        return;
    }
    QApplication::setOverrideCursor(themedCursor(Qt::WaitCursor));
    const bool done = session_->contentAwareFill(&error, r);
    QApplication::restoreOverrideCursor();
    if (!done) { QMessageBox::warning(this, windowTitle(), error); return; }
    recordAction("pixels.contentAwareFill", step);
    accept();
}

} // namespace app
