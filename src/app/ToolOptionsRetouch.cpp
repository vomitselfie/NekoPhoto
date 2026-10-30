// The retouching tools' options (ToolOptionsBar.h): Spot Healing and its J group, Clone Stamp, the Blur group
// and Dodge, Burn and Sponge.
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QHBoxLayout>
#include <algorithm>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

QWidget* ToolOptionsBar::buildHealingOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* mode = new QComboBox;
    mode->addItems({tr("Content-Aware"), tr("Create Texture"), tr("Proximity Match"), tr("Sampled (Healing Brush)"), tr("Patch"), tr("Content-Aware Move")});
    mode->setToolTip(tr("Sampled heals from where you Alt-click, like Clone Stamp; Patch: drag the selection to the area to copy from; "
                        "Content-Aware Move: drag the selection (or lasso one) to where it should go"));
    mode->setCurrentIndex(std::clamp(session_->spotHealingMode, 0, 5));
    h->addWidget(new QLabel(tr("Type")));
    h->addWidget(mode);
    // Content-Aware Move's own options, shown while it is the type.
    auto* moveOptions = new QWidget;
    auto* mh = new QHBoxLayout(moveOptions);
    mh->setContentsMargins(0, 0, 0, 0);
    auto* moveMode = new QComboBox;
    moveMode->addItems({tr("Move"), tr("Extend")});
    moveMode->setToolTip(tr("Move fills where the selection was; Extend leaves it and adds a copy"));
    moveMode->setCurrentIndex(session_->contentMoveExtend ? 1 : 0);
    connect(moveMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->contentMoveExtend = i == 1; });
    auto* adaptation = new QComboBox;
    adaptation->addItems({tr("Very Strict"), tr("Strict"), tr("Medium"), tr("Loose"), tr("Very Loose")});
    adaptation->setToolTip(tr("How far the moved patch adapts to its new place: its tone, and how wide a seam is blended"));
    adaptation->setCurrentIndex(std::clamp(session_->contentMoveAdaptation, 0, 4));
    connect(adaptation, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->contentMoveAdaptation = i; });
    mh->addWidget(new QLabel(tr("Mode")));
    mh->addWidget(moveMode);
    mh->addWidget(new QLabel(tr("Adaptation")));
    mh->addWidget(adaptation);
    h->addWidget(moveOptions);
    moveOptions->setVisible(session_->spotHealingMode == 5);
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, moveOptions](int i) { session_->spotHealingMode = i; moveOptions->setVisible(i == 5); });
    syncers_.push_back([this, mode] { if (mode->currentIndex() != session_->spotHealingMode) mode->setCurrentIndex(std::clamp(session_->spotHealingMode, 0, 5)); });
    addBrushTipFields(h);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildCloneOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* aligned = new QCheckBox(tr("Aligned"));
    aligned->setToolTip(tr("The source moves with the brush and keeps its offset between strokes"));
    aligned->setChecked(session_->cloneAligned);
    connect(aligned, &QCheckBox::toggled, this, [this](bool on) { session_->cloneAligned = on; });
    h->addWidget(aligned);
    auto* edgeAware = new QCheckBox(tr("Edge Aware"));
    edgeAware->setToolTip(tr("Follow the image: shading and texture stay in, edges between similar colours hold. "
                             "Change the tolerance right after a click to adjust that selection."));
    edgeAware->setChecked(session_->wandEdgeAware);
    connect(edgeAware, &QCheckBox::toggled, this, [this](bool on) { session_->wandEdgeAware = on; });
    h->addWidget(edgeAware);
    auto* refine = new QCheckBox(tr("Refine Edge"));
    refine->setToolTip(tr("Unmix the edge: the fringe of a line is partly selected, and Delete right after leaves the line its own colour, without a rim of the background's"));
    refine->setChecked(session_->wandRefineEdge);
    connect(refine, &QCheckBox::toggled, this, [this](bool on) { session_->wandRefineEdge = on; });
    h->addWidget(refine);
    auto* all = new QCheckBox(tr("Sample All Layers"));
    all->setChecked(session_->cloneSampleAll);
    connect(all, &QCheckBox::toggled, this, [this](bool on) { session_->cloneSampleAll = on; });
    h->addWidget(all);
    addBrushTipFields(h);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildSmudgeOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* mode = new QComboBox;
    mode->addItems({tr("Liquify"), tr("Blur"), tr("Smudge"), tr("Sharpen")});
    mode->setCurrentIndex(int(session_->blurMode));
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->blurMode = BlurToolMode(i); });
    syncers_.push_back([this, mode] { QSignalBlocker b(mode); mode->setCurrentIndex(int(session_->blurMode)); });
    h->addWidget(new QLabel(tr("Mode")));
    h->addWidget(mode);
    addBrushTipFields(h);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildToningOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* kind = new QComboBox;
    kind->addItems({tr("Dodge"), tr("Burn"), tr("Sponge")});
    kind->setCurrentIndex(int(session_->toning.kind));
    auto* range = new QComboBox;
    range->addItems({tr("Shadows"), tr("Midtones"), tr("Highlights")});
    range->setCurrentIndex(int(session_->toning.range));
    auto* protect = new QCheckBox(tr("Protect Tones"));
    protect->setChecked(session_->toning.protectTones);
    auto* sponge = new QComboBox;
    sponge->addItems({tr("Desaturate"), tr("Saturate")});
    sponge->setCurrentIndex(session_->toning.saturate ? 1 : 0);
    auto* rangeLabel = new QLabel(tr("Range"));
    auto* spongeLabel = new QLabel(tr("Mode"));
    auto show = [=, this] {
        const bool isSponge = session_->toning.kind == nekophoto::ToningKind::Sponge;
        rangeLabel->setVisible(!isSponge); range->setVisible(!isSponge); protect->setVisible(!isSponge);
        spongeLabel->setVisible(isSponge); sponge->setVisible(isSponge);
    };
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, show](int i) { session_->toning.kind = nekophoto::ToningKind(i); show(); });
    syncers_.push_back([this, kind, show] { { QSignalBlocker b(kind); kind->setCurrentIndex(int(session_->toning.kind)); } show(); });
    connect(range, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->toning.range = nekophoto::ToneRange(i); });
    connect(protect, &QCheckBox::toggled, this, [this](bool on) { session_->toning.protectTones = on; });
    connect(sponge, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->toning.saturate = i == 1; });
    h->addWidget(new QLabel(tr("Tool")));
    h->addWidget(kind);
    h->addWidget(rangeLabel);
    h->addWidget(range);
    h->addWidget(protect);
    h->addWidget(spongeLabel);
    h->addWidget(sponge);
    addBrushTipFields(h, tr("Exposure"));
    h->addStretch();
    show();
    return w;
}

} // namespace app
