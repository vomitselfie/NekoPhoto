// The Pen's and the Shape tool's options (ToolOptionsBar.h), and the path operation they share.
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include <QColorDialog>
#include "PresetLibrary.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

QComboBox* ToolOptionsBar::pathOperationBox(bool withNewLayer) {
    // Photoshop's path operations menu; New Layer only where an outline can start a layer of its own.
    // Short names in the bar, Photoshop's in the tooltips.
    auto* box = new QComboBox;
    box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    auto item = [box](const QString& shortName, const QString& name, int op) { box->addItem(shortName, op); box->setItemData(box->count() - 1, name, Qt::ToolTipRole); };
    if (withNewLayer) item(tr("New Layer"), tr("New Layer"), -1);
    item(tr("Combine"), tr("Combine Shapes"), int(VectorPath::Op::Add));
    item(tr("Subtract"), tr("Subtract Front Shape"), int(VectorPath::Op::Subtract));
    item(tr("Intersect"), tr("Intersect Shape Areas"), int(VectorPath::Op::Intersect));
    item(tr("Exclude"), tr("Exclude Overlapping Shapes"), int(VectorPath::Op::Xor));
    box->setToolTip(tr("How the next outline combines with the active shape layer (or the targeted vector mask, or the path)"));
    return box;
}

QWidget* ToolOptionsBar::buildPenOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* mode = new QComboBox;
    mode->addItems({tr("Shape"), tr("Path")});
    mode->setToolTip(tr("Shape makes a vector shape layer; Path draws into the Paths panel's path (or a new Work Path)"));
    // The Pen: the next outline's operation. Direct Selection: the picked subpath's component's.
    auto* op = pathOperationBox(true);
    auto* merge = new QPushButton(tr("Merge Shape Components"));
    merge->setToolTip(tr("Flatten the target path's components into one outline (curves become corner points)"));
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->penMode = i == 1 ? EditorSession::PenMode::Path : EditorSession::PenMode::Shape; });
    connect(op, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, op](int) {
        const int v = op->currentData().toInt();
        if (session_->tool() == Tool::DirectSelect) { if (v >= 0) session_->setSelectedSubpathOp(VectorPath::Op(v)); return; }
        session_->pathOp = v < 0 ? std::nullopt : std::optional(VectorPath::Op(v));
    });
    connect(merge, &QPushButton::clicked, this, [this] { session_->mergeTargetComponents(); });
    auto* autoAdd = new QCheckBox(tr("Auto Add/Delete"));
    autoAdd->setToolTip(tr("Click the target path's outline to add an anchor, an anchor to delete it"));
    autoAdd->setChecked(session_->penAutoAddDelete);
    connect(autoAdd, &QCheckBox::toggled, this, [this](bool on) { session_->penAutoAddDelete = on; });
    auto* title = new QLabel(tr("Pen"));
    syncers_.push_back([this, mode, op, title, autoAdd, merge] {
        QSignalBlocker b1(mode), b2(op);
        const bool direct = session_->tool() == Tool::DirectSelect;
        title->setText(direct ? tr("Direct Selection") : tr("Pen"));
        mode->setVisible(!direct);
        autoAdd->setVisible(!direct);
        mode->setCurrentIndex(session_->penMode == EditorSession::PenMode::Path ? 1 : 0);
        std::optional<VectorPath::Op> shown = session_->pathOp;
        bool enabled = true;
        if (direct) {
            auto path = session_->targetPath();
            const auto picked = session_->selectedSubpath();
            enabled = path && picked && *picked >= 0 && *picked < int(path->subpaths.size());
            shown = enabled ? std::optional(path->subpaths[size_t(*picked)].op) : std::nullopt;
            op->setToolTip(tr("The picked subpath's component: how it combines with the ones before it"));
        } else op->setToolTip(tr("How the next outline combines with the active shape layer (or the targeted vector mask, or the path)"));
        op->setEnabled(enabled);
        const int index = op->findData(shown ? int(*shown) : -1);
        op->setCurrentIndex(index >= 0 ? index : 0);
        merge->setEnabled(session_->targetPath().has_value());
    });
    h->addWidget(title);
    h->addWidget(mode);
    h->addWidget(op);
    h->addWidget(merge);
    h->addWidget(autoAdd);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildShapeOptions() {
    // New shapes take these settings; with a vector shape layer active, Fill and Stroke edit it (as Photoshop's bar does).
    QWidget* w = row();
    auto* h = layoutOf(w);
    ShapeToolSettings& st = session_->shapeTool;
    auto* kind = new QComboBox;
    kind->addItems({tr("Rectangle"), tr("Ellipse"), tr("Polygon"), tr("Line"), tr("Custom Shape")});
    h->addWidget(kind);
    // Per-kind fields.
    auto* radiusLabel = new QLabel(tr("Radius"));
    auto* radius = numberField(0, 5000, 0, " px", tr("Corner radius"));
    auto* sidesLabel = new QLabel(tr("Sides"));
    auto* sides = numberField(3, 100, 0, QString(), tr("Sides (points, for a star)"));
    sides->setValue(st.sides);
    auto* starLabel = new QLabel(tr("Star"));
    auto* star = numberField(0, 99, 0, "%", tr("How far a star's inner points come in; 0 for a polygon"));
    auto* weightLabel = new QLabel(tr("Weight"));
    auto* weight = numberField(0.5, 1000, 1, " px", tr("Line weight"));
    weight->setValue(st.lineWeight);
    auto* custom = new QComboBox;
    for (const auto& n : nekophoto::customShapeNames()) custom->addItem(QString::fromStdString(n));
    for (QWidget* x : std::initializer_list<QWidget*>{radiusLabel, radius, sidesLabel, sides, starLabel, star, weightLabel, weight, custom}) h->addWidget(x);
    auto showKind = [=, this] {
        const VectorShapeKind k = session_->shapeTool.kind;
        radiusLabel->setVisible(k == VectorShapeKind::Rectangle); radius->setVisible(k == VectorShapeKind::Rectangle);
        for (QWidget* x : {static_cast<QWidget*>(sidesLabel), static_cast<QWidget*>(sides), static_cast<QWidget*>(starLabel), static_cast<QWidget*>(star)}) x->setVisible(k == VectorShapeKind::Polygon);
        weightLabel->setVisible(k == VectorShapeKind::Line); weight->setVisible(k == VectorShapeKind::Line);
        custom->setVisible(k == VectorShapeKind::Custom);
    };
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, showKind](int i) { session_->shapeTool.kind = VectorShapeKind(i); showKind(); });
    connect(radius, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->shapeTool.cornerRadius = v; });
    connect(sides, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->shapeTool.sides = int(v); });
    connect(star, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->shapeTool.starInset = v / 100; });
    connect(weight, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->shapeTool.lineWeight = v; });
    connect(custom, &QComboBox::currentTextChanged, this, [this](const QString& n) { session_->shapeTool.custom = n.toStdString(); });
    // The path operation: a new layer, or a component of the active shape layer.
    auto* op = pathOperationBox(true);
    connect(op, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, op](int) {
        const int v = op->currentData().toInt();
        session_->pathOp = v < 0 ? std::nullopt : std::optional(VectorPath::Op(v));
    });
    auto* merge = new QToolButton;
    merge->setText(tr("Merge"));
    merge->setToolTip(tr("Merge Shape Components: flatten the active shape's components into one outline"));
    connect(merge, &QToolButton::clicked, this, [this] { session_->mergeTargetComponents(); });
    h->addWidget(op);
    h->addWidget(merge);
    h->addWidget(separator());

    // Fill and stroke.
    auto swatch = [](QToolButton* b, QColor c) { b->setStyleSheet(QStringLiteral("QToolButton { background: %1; min-width: 22px; border: 1px solid #888; }").arg(c.name())); };
    auto* fill = new QCheckBox(tr("Fill"));
    fill->setChecked(st.fill);
    auto* fillColour = new QToolButton;
    fillColour->setToolTip(tr("The active shape's fill colour (new shapes take the foreground colour)"));
    auto* stroke = new QCheckBox(tr("Stroke"));
    stroke->setChecked(st.stroke.enabled);
    auto* strokeColour = new QToolButton;
    strokeColour->setToolTip(tr("Stroke colour"));
    auto* strokeWidth = numberField(0, 1000, 1, " px", tr("Stroke width"));
    strokeWidth->setValue(st.stroke.width);
    auto* align = new QComboBox;
    align->addItems({tr("Inside"), tr("Center"), tr("Outside")});
    align->setCurrentIndex(1);
    auto* dash = new QComboBox;
    dash->addItems({tr("Solid"), tr("Dashed"), tr("Dotted")});
    // Colour, gradient or pattern, for the fill and for the stroke (Photoshop's fill-type picker): a gradient from
    // the presets (foreground to background by default), a pattern from the document's.
    auto* fillType = new QComboBox;
    fillType->addItems({tr("Color"), tr("Gradient"), tr("Pattern")});
    fillType->setToolTip(tr("Fill with a colour, a gradient or a pattern"));
    auto* fillSource = new QComboBox;
    fillSource->setToolTip(tr("The fill's gradient preset or document pattern"));
    auto* strokeType = new QComboBox;
    strokeType->addItems({tr("Color"), tr("Gradient"), tr("Pattern")});
    strokeType->setToolTip(tr("Stroke with a colour, a gradient or a pattern"));
    auto* strokeSource = new QComboBox;
    strokeSource->setToolTip(tr("The stroke's gradient preset or document pattern"));
    for (QComboBox* c : {fillType, fillSource, strokeType, strokeSource}) c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    fillSource->setMaximumWidth(180);
    strokeSource->setMaximumWidth(180);
    for (QWidget* x : std::initializer_list<QWidget*>{fill, fillType, fillColour, fillSource, stroke, strokeType, strokeColour, strokeSource, strokeWidth, align, dash}) h->addWidget(x);
    // The sources a type offers: gradient presets, or the document's patterns (kept when unchanged).
    auto sources = [this](QComboBox* type, QComboBox* source) {
        QSignalBlocker b(source);
        std::vector<std::pair<QString, QString>> items;   // data, label
        if (type->currentIndex() == 1) {
            items.push_back({QString(), tr("Foreground to Background")});
            for (const auto& g : PresetLibrary::instance().gradients()) items.push_back({QString::fromStdString(g.name), QString::fromStdString(g.name)});
        } else if (type->currentIndex() == 2 && session_->document())
            for (const auto& [id, name] : documentPatternList(*session_->document())) items.push_back({QString::fromStdString(id), QString::fromStdString(name)});
        bool same = int(items.size()) == source->count();
        for (int i = 0; same && i < source->count(); i++) same = source->itemData(i).toString() == items[size_t(i)].first && source->itemText(i) == items[size_t(i)].second;
        if (!same) {
            const QString kept = source->currentData().toString();
            source->clear();
            for (auto& [data, label] : items) source->addItem(label, data);
            const int at = source->findData(kept);
            if (at >= 0) source->setCurrentIndex(at);
        }
        if (type->currentIndex() == 2 && items.empty()) { source->addItem(tr("No patterns in the document")); source->setEnabled(false); }
        else source->setEnabled(true);
        source->setVisible(type->currentIndex() != 0);
    };
    auto paintOf = [this](QComboBox* type, QComboBox* source) {
        VectorPaint p;
        if (type->currentIndex() == 1) { p.kind = VectorPaint::Kind::Gradient; p.gradient = shapeGradient(source->currentData().toString(), session_->foregroundColor, session_->backgroundColor); }
        else if (type->currentIndex() == 2 && !source->currentData().toString().isEmpty()) { p.kind = VectorPaint::Kind::Pattern; p.pattern.id = source->currentData().toString().toStdString(); }
        return p;
    };
    // Each change goes to the settings and, when a vector shape layer is active, to it as one step.
    auto apply = [this](const QString& name, std::function<void(VectorShape&)> change) {
        if (auto shape = session_->activeVectorShape()) { change(*shape); session_->setActiveVectorShape(*shape, name); }
    };
    connect(fill, &QCheckBox::toggled, this, [this, apply](bool on) { session_->shapeTool.fill = on; apply(tr("Shape Fill"), [on](VectorShape& s) { s.fill = on; }); });
    connect(fillColour, &QToolButton::clicked, this, [this, apply, swatch, fillColour] {
        auto shape = session_->activeVectorShape();
        const QColor start = shape ? QColor(shape->r, shape->g, shape->b) : session_->foregroundColor;
        const QColor c = QColorDialog::getColor(start, this, tr("Fill"));
        if (!c.isValid()) return;
        swatch(fillColour, c);
        if (shape) apply(tr("Shape Fill"), [c](VectorShape& s) { s.r = uint8_t(c.red()); s.g = uint8_t(c.green()); s.b = uint8_t(c.blue()); s.fill = true; s.fillPaint = {}; });
        else { session_->foregroundColor = c; emit session_->toolChanged(); }
    });
    connect(stroke, &QCheckBox::toggled, this, [this, apply](bool on) { session_->shapeTool.stroke.enabled = on; apply(tr("Shape Stroke"), [on](VectorShape& s) { s.stroke.enabled = on; }); });
    connect(strokeColour, &QToolButton::clicked, this, [this, apply, swatch, strokeColour] {
        nekophoto::VectorStroke& t = session_->shapeTool.stroke;
        const QColor c = QColorDialog::getColor(QColor(t.r, t.g, t.b), this, tr("Stroke"));
        if (!c.isValid()) return;
        t.r = uint8_t(c.red()); t.g = uint8_t(c.green()); t.b = uint8_t(c.blue());
        swatch(strokeColour, c);
        apply(tr("Shape Stroke"), [c](VectorShape& s) { s.stroke.r = uint8_t(c.red()); s.stroke.g = uint8_t(c.green()); s.stroke.b = uint8_t(c.blue()); s.stroke.enabled = true; s.stroke.paint = {}; });
    });
    auto applyFillPaint = [this, apply, fillType, fillSource, paintOf] {
        const VectorPaint paint = paintOf(fillType, fillSource);
        session_->shapeTool.fillPaint = paint;
        apply(tr("Shape Fill"), [paint](VectorShape& s) { s.fillPaint = paint; s.fill = true; });
    };
    auto applyStrokePaint = [this, apply, strokeType, strokeSource, paintOf] {
        const VectorPaint paint = paintOf(strokeType, strokeSource);
        session_->shapeTool.stroke.paint = paint;
        apply(tr("Shape Stroke"), [paint](VectorShape& s) { s.stroke.paint = paint; s.stroke.enabled = true; });
    };
    connect(fillType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [sources, fillType, fillSource, fillColour, applyFillPaint](int i) {
        sources(fillType, fillSource); fillColour->setVisible(i == 0); applyFillPaint();
    });
    connect(fillSource, QOverload<int>::of(&QComboBox::activated), this, [applyFillPaint](int) { applyFillPaint(); });
    connect(strokeType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [sources, strokeType, strokeSource, strokeColour, applyStrokePaint](int i) {
        sources(strokeType, strokeSource); strokeColour->setVisible(i == 0); applyStrokePaint();
    });
    connect(strokeSource, QOverload<int>::of(&QComboBox::activated), this, [applyStrokePaint](int) { applyStrokePaint(); });
    connect(strokeWidth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, apply](double v) {
        session_->shapeTool.stroke.width = v; apply(tr("Shape Stroke"), [v](VectorShape& s) { s.stroke.width = v; });
    });
    connect(align, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, apply](int i) {
        const auto a = i == 0 ? nekophoto::VectorStroke::Align::Inside : i == 2 ? nekophoto::VectorStroke::Align::Outside : nekophoto::VectorStroke::Align::Center;
        session_->shapeTool.stroke.align = a; apply(tr("Shape Stroke"), [a](VectorShape& s) { s.stroke.align = a; });
    });
    connect(dash, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, apply](int i) {
        // Photoshop's presets, in stroke widths: dashes 4 on 2 off; dots 0 on 2 off with round caps.
        auto set = [i](nekophoto::VectorStroke& t) {
            t.dashes = i == 1 ? std::vector<double>{4, 2} : i == 2 ? std::vector<double>{0, 2} : std::vector<double>{};
            t.cap = i == 2 ? nekophoto::VectorStroke::Cap::Round : nekophoto::VectorStroke::Cap::Butt;
        };
        set(session_->shapeTool.stroke); apply(tr("Shape Stroke"), [set](VectorShape& s) { set(s.stroke); });
    });
    // Properties: the active shape's live rectangle or ellipse (Photoshop's Properties panel for a live shape).
    auto* propertiesLabel = new QLabel(tr("Properties"));
    auto* liveW = numberField(1, 300000, 1, " px", tr("Width"));
    auto* liveH = numberField(1, 300000, 1, " px", tr("Height"));
    auto* liveX = numberField(-300000, 300000, 1, " px", tr("Left"));
    auto* liveY = numberField(-300000, 300000, 1, " px", tr("Top"));
    std::array<QDoubleSpinBox*, 4> liveR{};
    const QString corners[4] = {tr("Top-left corner radius"), tr("Top-right corner radius"), tr("Bottom-right corner radius"), tr("Bottom-left corner radius")};
    for (int i = 0; i < 4; i++) liveR[size_t(i)] = numberField(0, 150000, 1, " px", corners[i]);
    h->addWidget(separator());
    h->addWidget(propertiesLabel);
    // Compact fields, each after a one-letter label (W, H, X, Y; the radii clockwise from the top left).
    std::vector<QLabel*> liveLabels;
    const char* names[] = {"W", "H", "X", "Y", "R"};
    int n = 0;
    for (QDoubleSpinBox* f : {liveW, liveH, liveX, liveY, liveR[0], liveR[1], liveR[2], liveR[3]}) {
        f->setKeyboardTracking(false);
        f->setSuffix(QString());
        f->setDecimals(1);
        f->setButtonSymbols(QAbstractSpinBox::NoButtons);
        f->setFixedWidth(56);
        if (n <= 4) { auto* label = new QLabel(tr(names[n])); liveLabels.push_back(label); h->addWidget(label); }
        h->addWidget(f);
        n++;
    }
    // The live shape shown: the one of the subpath Direct Selection picked, else the first.
    auto liveShown = [this]() -> std::optional<LiveShape> {
        const auto list = session_->activeLiveShapes();
        if (list.empty()) return std::nullopt;
        if (auto picked = session_->selectedSubpath(); picked)
            if (auto path = session_->targetPath(); path && *picked >= 0 && *picked < int(path->subpaths.size()))
                for (const LiveShape& l : list) if (l.group == path->subpaths[size_t(*picked)].group) return l;
        return list.front();
    };
    auto applyLive = [this, liveShown](std::function<void(LiveShape&)> change) {
        auto live = liveShown();
        if (!live) return;
        change(*live);
        session_->setActiveLiveShape(*live);
    };
    connect(liveW, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive](double v) { applyLive([v](LiveShape& l) { l.box.width = v; }); });
    connect(liveH, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive](double v) { applyLive([v](LiveShape& l) { l.box.height = v; }); });
    connect(liveX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive](double v) { applyLive([v](LiveShape& l) { l.box.x = v; }); });
    connect(liveY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive](double v) { applyLive([v](LiveShape& l) { l.box.y = v; }); });
    for (size_t i = 0; i < 4; i++)
        connect(liveR[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive, i](double v) { applyLive([v, i](LiveShape& l) { l.radii[i] = v; }); });
    // The bar shows the active shape's fill and stroke; otherwise the settings new shapes take.
    syncers_.push_back([=, this] {
        QSignalBlocker b1(kind), b2(fill), b3(stroke), b4(strokeWidth), b5(align), b6(dash), b7(op), b8(fillType), b9(strokeType);
        kind->setCurrentIndex(int(session_->shapeTool.kind));
        showKind();
        const int opIndex = op->findData(session_->pathOp ? int(*session_->pathOp) : -1);
        op->setCurrentIndex(opIndex >= 0 ? opIndex : 0);
        const auto shape = session_->activeVectorShape();
        merge->setEnabled(shape.has_value());
        const VectorPaint& fp = shape ? shape->fillPaint : session_->shapeTool.fillPaint;
        const VectorPaint& sp = shape ? shape->stroke.paint : session_->shapeTool.stroke.paint;
        fillType->setCurrentIndex(int(fp.kind));
        strokeType->setCurrentIndex(int(sp.kind));
        sources(fillType, fillSource);
        sources(strokeType, strokeSource);
        if (fp.kind == VectorPaint::Kind::Pattern) { QSignalBlocker b(fillSource); const int at = fillSource->findData(QString::fromStdString(fp.pattern.id)); if (at >= 0) fillSource->setCurrentIndex(at); }
        if (sp.kind == VectorPaint::Kind::Pattern) { QSignalBlocker b(strokeSource); const int at = strokeSource->findData(QString::fromStdString(sp.pattern.id)); if (at >= 0) strokeSource->setCurrentIndex(at); }
        fillColour->setVisible(fp.kind == VectorPaint::Kind::Solid);
        strokeColour->setVisible(sp.kind == VectorPaint::Kind::Solid);
        const auto live = liveShown();
        propertiesLabel->setVisible(live.has_value());
        for (size_t i = 0; i < liveLabels.size(); i++) liveLabels[i]->setVisible(live && (i < 4 || live->kind == LiveShape::Kind::Rectangle));
        for (QDoubleSpinBox* f : {liveW, liveH, liveX, liveY}) f->setVisible(live.has_value());
        for (QDoubleSpinBox* f : liveR) f->setVisible(live && live->kind == LiveShape::Kind::Rectangle);
        if (live) {
            propertiesLabel->setText(live->kind == LiveShape::Kind::Ellipse ? tr("Ellipse") : tr("Rectangle"));
            QSignalBlocker c1(liveW), c2(liveH), c3(liveX), c4(liveY), c5(liveR[0]), c6(liveR[1]), c7(liveR[2]), c8(liveR[3]);
            liveW->setValue(live->box.width); liveH->setValue(live->box.height); liveX->setValue(live->box.x); liveY->setValue(live->box.y);
            for (size_t i = 0; i < 4; i++) liveR[i]->setValue(live->radii[i]);
        }
        const nekophoto::VectorStroke& t = shape ? shape->stroke : session_->shapeTool.stroke;
        fill->setChecked(shape ? shape->fill : session_->shapeTool.fill);
        swatch(fillColour, shape ? QColor(shape->r, shape->g, shape->b) : session_->foregroundColor);
        stroke->setChecked(t.enabled);
        swatch(strokeColour, QColor(t.r, t.g, t.b));
        strokeWidth->setValue(t.width);
        align->setCurrentIndex(t.align == nekophoto::VectorStroke::Align::Inside ? 0 : t.align == nekophoto::VectorStroke::Align::Outside ? 2 : 1);
        dash->setCurrentIndex(t.dashes.empty() ? 0 : t.dashes.front() == 0 ? 2 : 1);
    });
    h->addStretch();
    return w;
}

} // namespace app
