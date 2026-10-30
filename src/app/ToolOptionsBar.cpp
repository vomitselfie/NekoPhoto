// The options bar (ToolOptionsBar.h): a page per tool, the one for the current tool shown; each group of tools' pages is
// built in ToolOptions<Group>.cpp.
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include <QCheckBox>
#include <QPushButton>
#include <QToolButton>
#include <QResizeEvent>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

ToolOptionsBar::ToolOptionsBar(EditorSession* session, CanvasWidget* canvas, QWidget* parent)
    : QToolBar(tr("Tool Options"), parent), session_(session), canvas_(canvas) {
    // One bar per tab; unique names keep QMainWindow::saveState quiet (their state is trivial, they can't move).
    static int count = 0;
    setObjectName(QStringLiteral("toolOptions%1").arg(++count));
    setMovable(false);
    setFloatable(false);
    stack_ = new QStackedWidget;
    stack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stack_->addWidget(buildMoveOptions());       // 0
    stack_->addWidget(buildBrushOptions());      // 1
    stack_->addWidget(buildMarqueeOptions());    // 2
    stack_->addWidget(buildLassoOptions());      // 3
    stack_->addWidget(buildWandOptions());       // 4
    stack_->addWidget(buildCropOptions());       // 5
    stack_->addWidget(buildZoomOptions());       // 6
    stack_->addWidget(buildEyedropperOptions()); // 7
    stack_->addWidget(buildHealingOptions());    // 8
    stack_->addWidget(buildCloneOptions());      // 9
    stack_->addWidget(buildSmudgeOptions());     // 10
    stack_->addWidget(buildGradientOptions());   // 11
    stack_->addWidget(buildShapeOptions());      // 12
    stack_->addWidget(buildTextOptions());       // 13
    stack_->addWidget(buildScribbleOptions());   // 14
    stack_->addWidget(buildToningOptions());     // 15
    stack_->addWidget(buildBucketOptions());     // 16
    stack_->addWidget(buildPenOptions());        // 17
    boxOptionsPage_ = stack_->count();
    stack_->addWidget(buildBoxOptions());        // the Artboard and Slice tools
    addWidget(stack_);
    connect(session_, &EditorSession::toolChanged, this, &ToolOptionsBar::syncTool);
    connect(session_, &EditorSession::transformChanged, this, &ToolOptionsBar::syncTransformFields);
    connect(session_, &EditorSession::layersChanged, this, &ToolOptionsBar::syncTransformFields);
    connect(session_, &EditorSession::layersChanged, this, [this] { for (auto& s : syncers_) s(); });
    syncTool();
}

void ToolOptionsBar::syncTool() {
    int index = 0;
    switch (session_->tool()) {
    case Tool::Move: index = 0; break;
    case Tool::Brush: index = 1; break;
    case Tool::Marquee: index = 2; break;
    case Tool::Lasso: index = 3; break;
    case Tool::Wand: index = 4; break;
    case Tool::Scribble: index = 14; break;
    case Tool::Crop: index = 5; break;
    case Tool::Zoom: case Tool::Hand: index = 6; break;
    case Tool::Eyedropper: index = 7; break;
    case Tool::SpotHealing: index = 8; break;
    case Tool::CloneStamp: index = 9; break;
    case Tool::Smudge: index = 10; break;
    case Tool::Gradient: index = 11; break;
    case Tool::Shape: index = 12; break;
    case Tool::Text: index = 13; break;
    case Tool::Dodge: index = 15; break;
    case Tool::PaintBucket: index = 16; break;
    case Tool::Pen: case Tool::DirectSelect: index = 17; break;
    case Tool::Artboard: case Tool::Slice: index = boxOptionsPage_; break;
    }
    for (auto& s : syncers_) s();
    stack_->setCurrentIndex(index);
    // Widgets that mirror session state.
    for (int page : {1, 8, 9, 10, 15, 16})
    for (auto* spin : stack_->widget(page)->findChildren<QDoubleSpinBox*>()) {
        QSignalBlocker b(spin);
        QString role = spin->property("role").toString();
        if (role == "size") spin->setValue(session_->brushSettings.diameter);
        else if (role == "hardness") spin->setValue(session_->brushSettings.hardness * 100);
        else if (role == "opacity") spin->setValue(session_->brushSettings.opacity * 100);
        else if (role == "smoothing") spin->setValue(session_->brushSmoothing.stabilizer);
    }
    // A MyPaint preset brings its own hardness.
    for (auto* spin : stack_->widget(1)->findChildren<QDoubleSpinBox*>())
        if (spin->property("role").toString() == "hardness") spin->setEnabled(session_->brushPreset.isEmpty());
    for (auto* b : stack_->widget(1)->findChildren<QToolButton*>()) {
        QString role = b->property("role").toString();
        if (role == "paint") b->setChecked(!session_->brushErase);
        else if (role == "erase") b->setChecked(session_->brushErase);
    }
    syncTransformFields();
}

void ToolOptionsBar::resizeEvent(QResizeEvent* e) {
    QToolBar::resizeEvent(e);
    int level = width() >= 1150 ? 0 : width() >= 900 ? 1 : 2;
    if (level != compact_) { compact_ = level; applyCompact(); }
}

void ToolOptionsBar::applyCompact() {
    wPercent_->setVisible(compact_ == 0);
    hPercent_->setVisible(compact_ == 0);
    transformFields_->setVisible(compact_ < 2);
    if (controlsCheck_) controlsCheck_->setText(compact_ ? tr("Controls") : tr("Transform Controls"));
    if (ratioCheck_) ratioCheck_->setText(compact_ ? tr("Ratio") : tr("Keep Ratio"));
}

QWidget* ToolOptionsBar::buildZoomOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* fit = new QPushButton(tr("Fit"));
    connect(fit, &QPushButton::clicked, this, [this] { session_->fitView(); });
    auto* actual = new QPushButton(tr("100%"));
    connect(actual, &QPushButton::clicked, this, [this] { session_->zoomTo(1); });
    h->addWidget(fit);
    h->addWidget(actual);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildEyedropperOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    h->addStretch();
    return w;
}

} // namespace app
