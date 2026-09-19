#include "Dialogs.h"
#include <QBuffer>
#include <QCheckBox>
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

namespace app {

std::optional<NewDocumentOptions> askNewDocument(QWidget* parent, NewDocumentOptions initial) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("New Canvas"));
    auto* form = new QFormLayout(&dialog);
    auto* width = new QSpinBox; width->setRange(1, 30000); width->setValue(initial.width); width->setSuffix(" px");
    auto* height = new QSpinBox; height->setRange(1, 30000); height->setValue(initial.height); height->setSuffix(" px");
    auto* resolution = new QDoubleSpinBox; resolution->setRange(1, 9600); resolution->setValue(initial.resolution); resolution->setSuffix(QObject::tr(" px/in"));
    form->addRow(QObject::tr("Width"), width);
    form->addRow(QObject::tr("Height"), height);
    form->addRow(QObject::tr("Resolution"), resolution);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return NewDocumentOptions{width->value(), height->value(), resolution->value()};
}

std::optional<CanvasSizeOptions> askCanvasSize(QWidget* parent, int currentWidth, int currentHeight) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Canvas Size"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* width = new QSpinBox; width->setRange(1, 30000); width->setValue(currentWidth); width->setSuffix(" px");
    auto* height = new QSpinBox; height->setRange(1, 30000); height->setValue(currentHeight); height->setSuffix(" px");
    form->addRow(QObject::tr("Current"), new QLabel(QStringLiteral("%1 × %2 px").arg(currentWidth).arg(currentHeight)));
    form->addRow(QObject::tr("Width"), width);
    form->addRow(QObject::tr("Height"), height);
    layout->addLayout(form);
    layout->addWidget(new QLabel(QObject::tr("Anchor")));
    auto* grid = new QGridLayout;
    double anchorX = 0.5, anchorY = 0.5;
    QList<QToolButton*> cells;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        auto* b = new QToolButton;
        b->setCheckable(true);
        b->setChecked(r == 1 && c == 1);
        b->setFixedSize(28, 28);
        b->setText(r == 1 && c == 1 ? QStringLiteral("●") : QStringLiteral("•"));
        QObject::connect(b, &QToolButton::clicked, &dialog, [&, r, c, b] { for (auto* o : cells) o->setChecked(o == b); anchorX = c / 2.0; anchorY = r / 2.0; });
        cells << b;
        grid->addWidget(b, r, c);
    }
    layout->addLayout(grid);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return CanvasSizeOptions{width->value(), height->value(), anchorX, anchorY};
}

std::optional<ImageSizeOptions> askImageSize(QWidget* parent, int currentWidth, int currentHeight, double currentResolution) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Image Size"));
    auto* form = new QFormLayout(&dialog);
    auto* width = new QSpinBox; width->setRange(1, 30000); width->setValue(currentWidth); width->setSuffix(" px");
    auto* height = new QSpinBox; height->setRange(1, 30000); height->setValue(currentHeight); height->setSuffix(" px");
    auto* resolution = new QDoubleSpinBox; resolution->setRange(1, 9600); resolution->setValue(currentResolution); resolution->setSuffix(QObject::tr(" px/in"));
    auto* lock = new QCheckBox(QObject::tr("Constrain proportions"));
    lock->setChecked(true);
    bool syncing = false;
    QObject::connect(width, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [&](int v) {
        if (syncing || !lock->isChecked()) return;
        syncing = true; height->setValue(std::max(1, int(std::lround(double(v) * currentHeight / currentWidth)))); syncing = false;
    });
    QObject::connect(height, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [&](int v) {
        if (syncing || !lock->isChecked()) return;
        syncing = true; width->setValue(std::max(1, int(std::lround(double(v) * currentWidth / currentHeight)))); syncing = false;
    });
    form->addRow(QObject::tr("Width"), width);
    form->addRow(QObject::tr("Height"), height);
    form->addRow(QObject::tr("Resolution"), resolution);
    form->addRow(lock);
    form->addRow(new QLabel(QObject::tr("Layers keep their full-resolution pixels; only their placement scales.")));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return ImageSizeOptions{width->value(), height->value(), resolution->value()};
}

std::optional<JpegOptions> askJpegExport(QWidget* parent, const QImage& flattened) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Export JPEG"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* preview = new QLabel;
    preview->setMinimumSize(480, 320);
    preview->setAlignment(Qt::AlignCenter);
    layout->addWidget(preview, 1);
    auto* form = new QFormLayout;
    auto* quality = new QSlider(Qt::Horizontal);
    quality->setRange(1, 100);
    quality->setValue(85);
    auto* qualityLabel = new QLabel("85");
    auto* qualityRow = new QWidget;
    auto* qh = new QHBoxLayout(qualityRow);
    qh->setContentsMargins(0, 0, 0, 0);
    qh->addWidget(quality, 1);
    qh->addWidget(qualityLabel);
    form->addRow(QObject::tr("Quality"), qualityRow);
    QColor background = Qt::white;
    auto* colorButton = new QPushButton(QObject::tr("White"));
    form->addRow(QObject::tr("Background"), colorButton);
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
        qualityLabel->setText(QString::number(quality->value()));
        double factor = double(flattened.width()) * flattened.height() / std::max(1, small.width() * small.height());
        sizeLabel->setText(QObject::tr("about %1 KB").arg(int(bytes.size() * factor / 1024)));
    };
    QObject::connect(quality, &QSlider::valueChanged, &dialog, [&] { refresh(); });
    QObject::connect(colorButton, &QPushButton::clicked, &dialog, [&] {
        QColor c = QColorDialog::getColor(background, &dialog, QObject::tr("Background"));
        if (c.isValid()) { background = c; colorButton->setText(c.name()); refresh(); }
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
