// The Crop tool's options and the Artboard and Slice tools' (ToolOptionsBar.h).
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include <QColorDialog>
#include "CanvasWidget.h"
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <algorithm>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

QWidget* ToolOptionsBar::buildCropOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    // Photoshop's crop presets: Ratio (free unless W and H are typed), Original Ratio, then the common ones.
    auto* ratio = new QComboBox;
    ratio->setToolTip(tr("The shape the crop box keeps"));
    struct Preset { const char* name; double w, h; };
    static const Preset presets[] = {{QT_TR_NOOP("Ratio"), 0, 0}, {QT_TR_NOOP("Original Ratio"), -1, -1}, {"1 : 1", 1, 1}, {"4 : 5 (8 : 10)", 4, 5},
                                     {"5 : 7", 5, 7}, {"2 : 3 (4 : 6)", 2, 3}, {"3 : 2", 3, 2}, {"4 : 3", 4, 3}, {"16 : 9", 16, 9}, {"9 : 16", 9, 16}};
    for (const Preset& p : presets) ratio->addItem(p.w == 0 || p.w < 0 ? tr(p.name) : QString::fromLatin1(p.name));
    auto ratioField = [](const QString& tip) {
        auto* f = numberField(0, 100000, 3, QString(), tip);
        f->setSpecialValueText(QStringLiteral(" "));   // 0 shows empty: no ratio
        f->setFixedWidth(64);
        return f;
    };
    auto* ratioW = ratioField(tr("Width of the ratio (empty for a free crop)"));
    auto* ratioH = ratioField(tr("Height of the ratio (empty for a free crop)"));
    auto* swapRatio = new QToolButton;
    swapRatio->setText(QStringLiteral("\u21c4"));
    swapRatio->setToolTip(tr("Swap height and width (X)"));
    auto* clearRatio = new QPushButton(tr("Clear"));
    clearRatio->setToolTip(tr("Clear the ratio"));
    auto applyFields = [this, ratioW, ratioH] { canvas_->setCropRatio(ratioW->value(), ratioH->value()); };
    connect(ratio, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i < 0 || i >= int(std::size(presets))) return;
        double w = presets[i].w, h = presets[i].h;
        if (w < 0) { w = session_->hasDocument() ? session_->document()->width : 0; h = session_->hasDocument() ? session_->document()->height : 0; }
        canvas_->setCropRatio(w, h);
    });
    connect(ratioW, &QDoubleSpinBox::editingFinished, this, applyFields);
    connect(ratioH, &QDoubleSpinBox::editingFinished, this, applyFields);
    connect(swapRatio, &QToolButton::clicked, this, [this] { canvas_->swapCropOrientation(); });
    connect(clearRatio, &QPushButton::clicked, this, [this, ratio] { if (ratio->currentIndex() == 0) canvas_->setCropRatio(0, 0); else ratio->setCurrentIndex(0); });
    // The fields and the preset follow the canvas (a preset, typed values, X).
    connect(canvas_, &CanvasWidget::cropRatioChanged, this, [this, ratio, ratioW, ratioH] {
        const double w = canvas_->cropRatioWidth(), h = canvas_->cropRatioHeight();
        { QSignalBlocker b1(ratioW), b2(ratioH); ratioW->setValue(w); ratioH->setValue(h); }
        int match = 0;
        for (int i = 2; i < int(std::size(presets)); i++) if (presets[i].w == w && presets[i].h == h) match = i;
        if (match == 0 && w > 0 && session_->hasDocument() && w == session_->document()->width && h == session_->document()->height) match = 1;
        QSignalBlocker b(ratio);
        ratio->setCurrentIndex(match);
    });
    h->addWidget(ratio);
    h->addWidget(ratioW);
    h->addWidget(swapRatio);
    h->addWidget(ratioH);
    h->addWidget(clearRatio);
    auto* apply = new QPushButton(tr("Crop"));
    connect(apply, &QPushButton::clicked, this, [this] { canvas_->applyCrop(); });
    auto* cancel = new QPushButton(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, [this] { canvas_->cancelCrop(); });
    h->addWidget(apply);
    h->addWidget(cancel);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildBoxOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* title = new QLabel;
    // Artboard: the active artboard's background, as Photoshop's Artboard options.
    auto* backgroundLabel = new QLabel(tr("Background"));
    auto* background = new QComboBox;
    background->addItems({tr("White"), tr("Black"), tr("Transparent"), tr("Other…")});
    background->setToolTip(tr("The active artboard's background"));
    connect(background, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        const Layer* l = session_->activeLayer();
        if (!l || !l->artboard) return;
        Artboard a = *l->artboard;
        a.background = i + 1;
        if (a.background == Artboard::Other) {
            const QColor c = QColorDialog::getColor(QColor::fromRgbF(float(a.red), float(a.green), float(a.blue)), this, tr("Artboard Background"));
            if (!c.isValid()) { emit session_->layersChanged(); return; }
            a.red = c.redF(); a.green = c.greenF(); a.blue = c.blueF();
        }
        session_->setArtboard(l->id, a, false);
    });
    // Slice: remove them all.
    auto* clear = new QPushButton(tr("Delete All Slices"));
    connect(clear, &QPushButton::clicked, this, [this] {
        if (!session_->document()) return;
        std::vector<uint32_t> ids;
        for (const Slice& s : session_->document()->slices) ids.push_back(s.id);
        if (ids.empty()) return;
        session_->beginEdit(QT_TRANSLATE_NOOP("History", "Delete Slices"));
        for (uint32_t id : ids) session_->deleteSlice(id);
        session_->endEdit();
    });
    syncers_.push_back([this, title, backgroundLabel, background, clear] {
        const bool artboard = session_->tool() == Tool::Artboard;
        title->setText(artboard ? tr("Artboard") : tr("Slice"));
        const Layer* l = session_->activeLayer();
        const bool onArtboard = artboard && l && l->artboard;
        backgroundLabel->setVisible(artboard);
        background->setVisible(artboard);
        background->setEnabled(onArtboard);
        if (onArtboard) { QSignalBlocker b(background); background->setCurrentIndex(std::clamp(l->artboard->background, 1, 4) - 1); }
        clear->setVisible(!artboard);
    });
    h->addWidget(title);
    h->addWidget(backgroundLabel);
    h->addWidget(background);
    h->addWidget(clear);
    h->addStretch();
    return w;
}

} // namespace app
