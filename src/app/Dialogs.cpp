#include "Dialogs.h"
#include "Icons.h"
#include "Style.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QImageWriter>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace app {

namespace {

QSpinBox* pixelField(int value) {
    auto* f = new QSpinBox;
    f->setRange(1, 30000);
    f->setValue(value);
    f->setSuffix(" px");
    f->setMinimumWidth(110);
    return f;
}

QDoubleSpinBox* resolutionField(double value) {
    auto* f = new QDoubleSpinBox;
    f->setRange(1, 9600);
    f->setDecimals(1);
    f->setValue(value);
    f->setSuffix(QObject::tr(" px/in"));
    f->setMinimumWidth(110);
    return f;
}

QLabel* hintLabel(const QString& text = {}) {
    auto* l = new QLabel(text);
    l->setStyleSheet(hintStyle());
    l->setWordWrap(true);
    return l;
}

QString printSize(int width, int height, double resolution) {
    return QObject::tr("%1 × %2 in at %3 px/in").arg(width / resolution, 0, 'f', 2).arg(height / resolution, 0, 'f', 2).arg(resolution, 0, 'f', resolution == std::floor(resolution) ? 0 : 1);
}

QString megapixels(int width, int height) {
    double mp = double(width) * height / 1e6;
    return QObject::tr("%1 × %2 px, %3 MP").arg(width).arg(height).arg(mp, 0, 'f', mp < 10 ? 1 : 0);
}

QDialogButtonBox* okCancel(QDialog* dialog, const QString& okText = {}) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    if (!okText.isEmpty()) buttons->button(QDialogButtonBox::Ok)->setText(okText);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

} // namespace

std::optional<NewDocumentOptions> askNewDocument(QWidget* parent, NewDocumentOptions initial) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("New Canvas"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    struct Preset { QString name; int width, height; double resolution; };
    QList<Preset> presets{
        {QObject::tr("Custom"), 0, 0, 0},
        {QObject::tr("HD 1920 × 1080"), 1920, 1080, 72},
        {QObject::tr("4K UHD 3840 × 2160"), 3840, 2160, 72},
        {QObject::tr("Square 2048 × 2048"), 2048, 2048, 72},
        {QObject::tr("Portrait 1080 × 1350"), 1080, 1350, 72},
        {QObject::tr("A4 at 300 px/in"), 2480, 3508, 300},
        {QObject::tr("US Letter at 300 px/in"), 2550, 3300, 300},
    };
    QImage clip = QApplication::clipboard()->image();
    if (!clip.isNull()) presets.insert(1, {QObject::tr("Clipboard %1 × %2").arg(clip.width()).arg(clip.height()), clip.width(), clip.height(), initial.resolution});
    auto* preset = new QComboBox;
    for (auto& p : presets) preset->addItem(p.name);
    form->addRow(QObject::tr("Preset"), preset);

    auto* width = pixelField(initial.width);
    auto* height = pixelField(initial.height);
    auto* resolution = resolutionField(initial.resolution);
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(width);
    auto* swap = new QToolButton;
    swap->setIcon(toolIcon("arrow-left-right", 16));
    swap->setToolTip(QObject::tr("Swap width and height"));
    swap->setAutoRaise(true);
    sizeRow->addWidget(swap);
    sizeRow->addWidget(height);
    form->addRow(QObject::tr("Width / Height"), sizeRow);
    form->addRow(QObject::tr("Resolution"), resolution);
    layout->addLayout(form);
    auto* summary = hintLabel();
    layout->addWidget(summary);

    bool syncing = false;
    auto refresh = [&] { summary->setText(megapixels(width->value(), height->value()) + "\n" + printSize(width->value(), height->value(), resolution->value())); };
    auto custom = [&] { if (!syncing) { syncing = true; preset->setCurrentIndex(0); syncing = false; } refresh(); };
    QObject::connect(width, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, custom);
    QObject::connect(height, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, custom);
    QObject::connect(resolution, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog, custom);
    QObject::connect(preset, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int i) {
        if (syncing || i <= 0) return;
        syncing = true;
        width->setValue(presets[i].width);
        height->setValue(presets[i].height);
        resolution->setValue(presets[i].resolution);
        syncing = false;
        refresh();
    });
    QObject::connect(swap, &QToolButton::clicked, &dialog, [&] { int w = width->value(); syncing = true; width->setValue(height->value()); height->setValue(w); syncing = false; custom(); });
    refresh();

    layout->addWidget(okCancel(&dialog, QObject::tr("Create")));
    width->selectAll();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return NewDocumentOptions{width->value(), height->value(), resolution->value()};
}

std::optional<CanvasSizeOptions> askCanvasSize(QWidget* parent, int currentWidth, int currentHeight) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Canvas Size"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    form->addRow(QObject::tr("Current size"), new QLabel(QStringLiteral("%1 × %2 px").arg(currentWidth).arg(currentHeight)));
    auto* width = pixelField(currentWidth);
    auto* height = pixelField(currentHeight);
    auto* relative = new QCheckBox(QObject::tr("Relative"));
    relative->setToolTip(QObject::tr("Enter how much to add (or, negative, remove) instead of the new size"));
    form->addRow(QObject::tr("Width"), width);
    form->addRow(QObject::tr("Height"), height);
    form->addRow(QString(), relative);
    layout->addLayout(form);

    // Anchor: the fixed corner or edge, shown as arrows pointing where the canvas grows.
    auto* anchorRow = new QHBoxLayout;
    anchorRow->addWidget(new QLabel(QObject::tr("Anchor")));
    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    int anchorRowIndex = 1, anchorCol = 1;
    QList<QToolButton*> cells;
    auto relabel = [&] {
        static const char* arrows[3][3] = {{"↖", "↑", "↗"}, {"←", "●", "→"}, {"↙", "↓", "↘"}};
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            int dr = std::clamp(r - anchorRowIndex, -1, 1), dc = std::clamp(c - anchorCol, -1, 1);
            auto* b = cells[r * 3 + c];
            b->setText(QString::fromUtf8(arrows[dr + 1][dc + 1]));
            b->setChecked(r == anchorRowIndex && c == anchorCol);
        }
    };
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        auto* b = new QToolButton;
        b->setCheckable(true);
        b->setFixedSize(30, 30);
        b->setAutoRaise(true);
        QObject::connect(b, &QToolButton::clicked, &dialog, [&, r, c] { anchorRowIndex = r; anchorCol = c; relabel(); });
        cells << b;
        grid->addWidget(b, r, c);
    }
    relabel();
    anchorRow->addLayout(grid);
    anchorRow->addStretch();
    layout->addLayout(anchorRow);
    auto* summary = hintLabel();
    layout->addWidget(summary);

    auto newSize = [&](int& w, int& h) {
        w = relative->isChecked() ? currentWidth + width->value() : width->value();
        h = relative->isChecked() ? currentHeight + height->value() : height->value();
    };
    auto refresh = [&] {
        int w, h; newSize(w, h);
        summary->setText(QObject::tr("New size %1 × %2 px (%3%4 × %5%6)").arg(w).arg(h)
            .arg(w - currentWidth >= 0 ? "+" : "").arg(w - currentWidth).arg(h - currentHeight >= 0 ? "+" : "").arg(h - currentHeight));
    };
    QObject::connect(relative, &QCheckBox::toggled, &dialog, [&](bool on) {
        QSignalBlocker a(width), b(height);
        if (on) { width->setRange(1 - currentWidth, 30000 - currentWidth); height->setRange(1 - currentHeight, 30000 - currentHeight); width->setValue(0); height->setValue(0); }
        else { width->setRange(1, 30000); height->setRange(1, 30000); width->setValue(currentWidth); height->setValue(currentHeight); }
        refresh();
    });
    QObject::connect(width, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, refresh);
    QObject::connect(height, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, refresh);
    refresh();

    layout->addWidget(okCancel(&dialog));
    width->selectAll();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    int w, h; newSize(w, h);
    return CanvasSizeOptions{std::max(1, w), std::max(1, h), anchorCol / 2.0, anchorRowIndex / 2.0};
}

std::optional<ImageSizeOptions> askImageSize(QWidget* parent, int currentWidth, int currentHeight, double currentResolution) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Image Size"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    form->addRow(QObject::tr("Current size"), new QLabel(megapixels(currentWidth, currentHeight)));
    auto* scale = new QDoubleSpinBox;
    scale->setRange(0.1, 3000);
    scale->setDecimals(2);
    scale->setValue(100);
    scale->setSuffix("%");
    scale->setMinimumWidth(110);
    auto* width = pixelField(currentWidth);
    auto* height = pixelField(currentHeight);
    auto* resolution = resolutionField(currentResolution);
    auto* lock = new QCheckBox(QObject::tr("Constrain proportions"));
    lock->setChecked(true);
    form->addRow(QObject::tr("Scale"), scale);
    form->addRow(QObject::tr("Width"), width);
    form->addRow(QObject::tr("Height"), height);
    form->addRow(QString(), lock);
    form->addRow(QObject::tr("Resolution"), resolution);
    auto* sampling = new QComboBox;
    sampling->addItems({QObject::tr("Nearest Neighbour (hard pixels)"), QObject::tr("Smooth (bilinear)"), QObject::tr("High Quality (area average, best for reducing)")});
    sampling->setCurrentIndex(2);
    form->addRow(QObject::tr("Resampling"), sampling);
    layout->addLayout(form);
    auto* summary = hintLabel();
    layout->addWidget(summary);
    layout->addWidget(hintLabel(QObject::tr("Every layer's pixels and mask are resampled; the layout scales with them.")));

    bool syncing = false;
    auto refresh = [&] { summary->setText(QObject::tr("New size %1\n%2").arg(megapixels(width->value(), height->value()), printSize(width->value(), height->value(), resolution->value()))); };
    auto setScaleFromWidth = [&] { QSignalBlocker b(scale); scale->setValue(100.0 * width->value() / currentWidth); };
    QObject::connect(width, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [&](int v) {
        if (syncing) return;
        syncing = true;
        if (lock->isChecked()) height->setValue(std::max(1, int(std::lround(double(v) * currentHeight / currentWidth))));
        setScaleFromWidth();
        syncing = false;
        refresh();
    });
    QObject::connect(height, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [&](int v) {
        if (syncing) return;
        syncing = true;
        if (lock->isChecked()) { width->setValue(std::max(1, int(std::lround(double(v) * currentWidth / currentHeight)))); setScaleFromWidth(); }
        syncing = false;
        refresh();
    });
    QObject::connect(scale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog, [&](double pct) {
        if (syncing) return;
        syncing = true;
        width->setValue(std::max(1, int(std::lround(currentWidth * pct / 100))));
        height->setValue(std::max(1, int(std::lround(currentHeight * pct / 100))));
        syncing = false;
        refresh();
    });
    QObject::connect(resolution, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog, refresh);
    refresh();

    layout->addWidget(okCancel(&dialog, QObject::tr("Resize")));
    width->selectAll();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return ImageSizeOptions{width->value(), height->value(), resolution->value(), sampling->currentIndex()};
}

std::optional<JpegOptions> askJpegExport(QWidget* parent, const QImage& flattened) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Export JPEG"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* preview = new QLabel;
    preview->setMinimumSize(560, 360);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet(QStringLiteral("background: %1;").arg(QColor(46, 46, 46).name()));
    layout->addWidget(preview, 1);
    auto* form = new QFormLayout;
    auto* quality = new QSlider(Qt::Horizontal);
    quality->setRange(1, 100);
    quality->setValue(85);
    auto* qualitySpin = new QSpinBox;
    qualitySpin->setRange(1, 100);
    qualitySpin->setValue(85);
    qualitySpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    qualitySpin->setFixedWidth(48);
    qualitySpin->setAlignment(Qt::AlignRight);
    auto* qualityRow = new QHBoxLayout;
    qualityRow->addWidget(quality, 1);
    qualityRow->addWidget(qualitySpin);
    form->addRow(QObject::tr("Quality"), qualityRow);
    QColor background = Qt::white;
    auto* colorButton = new QPushButton;
    auto* colorSwatch = new QLabel;
    colorSwatch->setFixedSize(22, 22);
    colorSwatch->setAutoFillBackground(true);
    auto swatch = [&] {
        colorSwatch->setStyleSheet(QStringLiteral("background: %1; border: 1px solid rgba(0,0,0,120);").arg(background.name()));
        colorButton->setText(background == Qt::white ? QObject::tr("White") : background == Qt::black ? QObject::tr("Black") : background.name());
    };
    swatch();
    auto* colorRow = new QHBoxLayout;
    colorRow->addWidget(colorSwatch);
    colorRow->addWidget(colorButton);
    colorRow->addStretch();
    form->addRow(QObject::tr("Behind transparency"), colorRow);
    auto* sizeLabel = new QLabel;
    form->addRow(QObject::tr("File size"), sizeLabel);
    layout->addLayout(form);
    QImage small = flattened.width() > 1000 || flattened.height() > 1000 ? flattened.scaled(1000, 1000, Qt::KeepAspectRatio, Qt::SmoothTransformation) : flattened;
    auto refresh = [&] {
        QImage flat(small.size(), QImage::Format_RGB32);
        flat.fill(background);
        QPainter p(&flat);
        p.drawImage(0, 0, small);
        p.end();
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "jpeg");
        writer.setQuality(quality->value());
        writer.write(flat);
        QImage decoded = QImage::fromData(bytes, "jpeg");
        preview->setPixmap(QPixmap::fromImage(decoded.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        double factor = double(flattened.width()) * flattened.height() / std::max(1, small.width() * small.height());
        double kb = bytes.size() * factor / 1024;
        sizeLabel->setText(kb >= 1024 ? QObject::tr("about %1 MB").arg(kb / 1024, 0, 'f', 1) : QObject::tr("about %1 KB").arg(int(kb)));
    };
    QObject::connect(quality, &QSlider::valueChanged, &dialog, [&](int v) { { QSignalBlocker b(qualitySpin); qualitySpin->setValue(v); } refresh(); });
    QObject::connect(qualitySpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [&](int v) { quality->setValue(v); });
    QObject::connect(colorButton, &QPushButton::clicked, &dialog, [&] {
        QColor c = QColorDialog::getColor(background, &dialog, QObject::tr("Colour behind transparent areas"));
        if (c.isValid()) { background = c; swatch(); refresh(); }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    refresh();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return JpegOptions{quality->value(), background};
}

} // namespace app
