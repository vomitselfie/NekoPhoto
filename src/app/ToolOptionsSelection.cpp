// The selection tools' options (ToolOptionsBar.h): the Marquee, Lasso, Magic Wand and Quick Select, with the
// selection modes and Feather they share.
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include "CanvasWidget.h"
#include "Icons.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include "ModelStore.h"
#include <QToolButton>
#include <QHBoxLayout>
#include <QSpinBox>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

QWidget* ToolOptionsBar::buildMarqueeOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* kind = new QComboBox;
    kind->addItems({tr("Rectangle"), tr("Ellipse")});
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->marqueeKind = i == 1 ? MarqueeKind::Ellipse : MarqueeKind::Rectangle; });
    syncers_.push_back([this, kind] { QSignalBlocker b(kind); kind->setCurrentIndex(session_->marqueeKind == MarqueeKind::Ellipse ? 1 : 0); });
    h->addWidget(kind);
    auto* aa = new QCheckBox(tr("Anti-alias"));
    aa->setChecked(session_->selectionAntialiased);
    connect(aa, &QCheckBox::toggled, this, [this](bool on) { session_->selectionAntialiased = on; });
    h->addWidget(aa);
    addFeatherField(h);
    h->addWidget(separator());
    addSelectionModeButtons(h);
    h->addStretch();
    return w;
}

void ToolOptionsBar::addSelectionModeButtons(QHBoxLayout* h) {
    // Photoshop's four buttons: what a new shape does with the selection (Shift, Alt and Shift+Alt for one use).
    struct Mode { SelectionMode mode; const char* icon; QString tip; };
    const Mode modes[] = {{SelectionMode::Replace, "square", tr("New Selection")}, {SelectionMode::Add, "union", tr("Add to Selection (Shift)")},
                          {SelectionMode::Subtract, "subtract", tr("Subtract from Selection (Alt)")}, {SelectionMode::Intersect, "intersect", tr("Intersect with Selection (Shift+Alt)")}};
    auto* group = new QButtonGroup(h->parentWidget());
    group->setExclusive(true);
    for (const Mode& m : modes) {
        auto* b = new QToolButton;
        b->setIcon(toolIcon(QString::fromLatin1(m.icon), 16));
        b->setToolTip(m.tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setProperty("role", QStringLiteral("selectionMode%1").arg(int(m.mode)));
        group->addButton(b);
        const SelectionMode mode = m.mode;
        b->setChecked(session_->selectionToolMode == mode);
        connect(b, &QToolButton::toggled, this, [this, mode](bool on) { if (on && session_->selectionToolMode != mode) { session_->selectionToolMode = mode; emit session_->toolChanged(); } });
        syncers_.push_back([this, b, mode] { QSignalBlocker blocker(b); b->setChecked(session_->selectionToolMode == mode); });
        h->addWidget(b);
    }
}

void ToolOptionsBar::addFeatherField(QHBoxLayout* h) {
    h->addWidget(new QLabel(tr("Feather")));
    auto* feather = numberField(0, 250, 0, " px", tr("Softens the edge of each new selection by this radius"));
    feather->setProperty("role", "feather");
    feather->setValue(session_->selectionToolFeather);
    connect(feather, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->selectionToolFeather = v; emit session_->toolChanged(); });
    syncers_.push_back([this, feather] { QSignalBlocker b(feather); feather->setValue(session_->selectionToolFeather); });
    h->addWidget(feather);
}

QWidget* ToolOptionsBar::buildLassoOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* kind = new QComboBox;
    kind->addItems({tr("Freehand"), tr("Polygonal")});
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->lassoKind = i == 1 ? LassoKind::Polygonal : LassoKind::Freehand; canvas_->cancelLasso(); emit session_->toolChanged(); });
    kind->setProperty("role", "lassoKind");
    syncers_.push_back([this, kind] { QSignalBlocker b(kind); kind->setCurrentIndex(session_->lassoKind == LassoKind::Polygonal ? 1 : 0); });
    h->addWidget(kind);
    addFeatherField(h);
    h->addWidget(separator());
    addSelectionModeButtons(h);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildScribbleOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    h->addWidget(new QLabel(tr("Engine")));
    auto* engine = new QComboBox;
    engine->addItems({tr("Scribble"), tr("Click")});
    engine->setToolTip(tr("Scribble: strokes segmented by GrabCut, no model. Click: EfficientSAM finds the object under a click; Alt-click marks what is not it, a drag draws a box; up to six prompts count"));
    engine->setCurrentIndex(session_->quickSelectClicks ? 1 : 0);
    h->addWidget(engine);
    auto* fetch = new QPushButton(tr("Download model (48 MB)"));
    fetch->setToolTip(tr("Meta's EfficientSAM as packaged by OpenCV's model zoo (Apache-2.0), kept in the models folder next to the Remove Background ones; nothing is uploaded"));
    fetch->setVisible(session_->quickSelectClicks && !ModelStore::promptReady());
    connect(engine, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, fetch](int i) { session_->setQuickSelectClicks(i == 1); fetch->setVisible(session_->quickSelectClicks && !ModelStore::promptReady()); });
    connect(fetch, &QPushButton::clicked, this, [this, fetch] {
        fetch->setEnabled(false);
        ModelStore::download(ModelStore::promptModel(), fetch,
            [fetch](qint64 received, qint64 total) { fetch->setText(tr("Downloading… %1%").arg(total > 0 ? int(received * 100 / total) : 0)); },
            [this, fetch](QString, QString error) {
                fetch->setEnabled(true);
                fetch->setText(tr("Download model (48 MB)"));
                if (!error.isEmpty()) QMessageBox::warning(this, tr("Download failed"), error);
                fetch->setVisible(session_->quickSelectClicks && !ModelStore::promptReady());
            });
    });
    h->addWidget(fetch);
    h->addWidget(new QLabel(tr("Mode")));
    auto* mode = new QComboBox;
    mode->addItems({tr("Subject"), tr("Background")});
    mode->setToolTip(tr("What a stroke marks; Alt flips it for one stroke"));
    mode->setCurrentIndex(session_->scribbleBackground ? 1 : 0);
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->scribbleBackground = i == 1; });
    syncers_.push_back([this, mode] { QSignalBlocker b(mode); mode->setCurrentIndex(session_->scribbleBackground ? 1 : 0); });
    h->addWidget(mode);
    h->addWidget(new QLabel(tr("Size")));
    auto* size = new QSpinBox;
    size->setRange(1, 500);
    size->setButtonSymbols(QAbstractSpinBox::NoButtons);
    size->setAlignment(Qt::AlignRight);
    size->setFixedWidth(48);
    size->setValue(session_->scribbleSize);
    connect(size, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { session_->scribbleSize = v; });
    syncers_.push_back([this, size] { QSignalBlocker b(size); size->setValue(session_->scribbleSize); });
    h->addWidget(size);
    h->addWidget(new QLabel(tr("Refine")));
    auto* refine = new QSpinBox;
    refine->setRange(0, 40);
    refine->setToolTip(tr("Pulls the selection onto the image's own edges by this many pixels; 0 keeps the segmentation as it is"));
    refine->setButtonSymbols(QAbstractSpinBox::NoButtons);
    refine->setAlignment(Qt::AlignRight);
    refine->setFixedWidth(40);
    refine->setValue(session_->scribbleRefine);
    connect(refine, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { session_->scribbleRefine = v; session_->startQuickSelectJob(); });
    h->addWidget(refine);
    auto* clear = new QPushButton(tr("Clear"));
    clear->setToolTip(tr("Forgets the strokes and clicks (Esc); Backspace takes back the last one"));
    connect(clear, &QPushButton::clicked, this, [this] { session_->clearScribbles(); session_->clearClickPrompts(); });
    h->addWidget(clear);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildWandOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    h->addWidget(new QLabel(tr("Tolerance")));
    auto* tol = new QSpinBox;
    tol->setRange(0, 255);
    tol->setButtonSymbols(QAbstractSpinBox::NoButtons);
    tol->setAlignment(Qt::AlignRight);
    tol->setFixedWidth(48);
    tol->setValue(session_->wandTolerance);
    // Right after a click, a new tolerance redoes that click's selection at once (the field is kept).
    connect(tol, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { session_->wandTolerance = v; if (session_->wandEdgeAware) session_->retolerateWand(v); });
    syncers_.push_back([this, tol] { QSignalBlocker b(tol); tol->setValue(session_->wandTolerance); });
    h->addWidget(tol);
    auto* contiguous = new QCheckBox(tr("Contiguous"));
    contiguous->setChecked(session_->wandContiguous);
    connect(contiguous, &QCheckBox::toggled, this, [this](bool on) { session_->wandContiguous = on; });
    syncers_.push_back([this, contiguous] { QSignalBlocker b(contiguous); contiguous->setChecked(session_->wandContiguous); });
    h->addWidget(contiguous);
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
    all->setChecked(session_->wandSampleAll);
    connect(all, &QCheckBox::toggled, this, [this](bool on) { session_->wandSampleAll = on; });
    h->addWidget(all);
    h->addWidget(new QLabel(tr("Sample")));
    auto* sample = new QComboBox;
    sample->addItems({tr("Point Sample"), tr("3 by 3 Average"), tr("5 by 5 Average")});
    sample->setCurrentIndex(session_->wandSampleRadius);
    connect(sample, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->wandSampleRadius = i; });
    h->addWidget(sample);
    h->addWidget(separator());
    addSelectionModeButtons(h);
    h->addStretch();
    return w;
}

} // namespace app
