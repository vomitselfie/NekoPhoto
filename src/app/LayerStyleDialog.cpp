#include "LayerStyleDialog.h"
#include "PresetLibrary.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

namespace {

const char* const kEffectNames[] = {"Bevel & Emboss", "Stroke", "Inner Shadow", "Inner Glow", "Satin", "Color Overlay",
                                    "Gradient Overlay", "Pattern Overlay", "Outer Glow", "Drop Shadow"};

/// EffectBlend's order is Photoshop's menu order.
const char* const kBlendNames[] = {"Normal", "Dissolve", "Darken", "Multiply", "Color Burn", "Linear Burn", "Darker Color", "Lighten", "Screen",
                                   "Color Dodge", "Linear Dodge (Add)", "Lighter Color", "Overlay", "Soft Light", "Hard Light", "Vivid Light",
                                   "Linear Light", "Pin Light", "Hard Mix", "Difference", "Exclusion", "Subtract", "Divide", "Hue", "Saturation",
                                   "Color", "Luminosity"};
constexpr int kBlendCount = int(std::size(kBlendNames));

void paintSwatch(QPushButton* button, StyleColor c) {
    button->setStyleSheet(QStringLiteral("background-color: rgb(%1,%2,%3); min-width: 48px;").arg(c.r).arg(c.g).arg(c.b));
}

/// The effect's first instance, added (switched off) when the layer has none, so its page has something to edit.
template <typename T>
T* firstOf(std::vector<T>& list, bool& madeUp) {
    if (list.empty()) { list.push_back(T{}); list.back().enabled = false; madeUp = true; }
    return &list.front();
}

} // namespace

LayerStyleDialog::LayerStyleDialog(EditorSession* session, const Uuid& layer, QWidget* parent, int page)
    : QDialog(parent), session_(session), layer_(layer) {
    setWindowTitle(tr("Layer Style"));
    if (!session || !session->document() || !session->beginLayerStyleEdit(layer)) {
        auto* box = new QVBoxLayout(this);
        box->addWidget(new QLabel(tr("This layer cannot have effects."), this));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        box->addWidget(buttons);
        finished_ = true;
        return;
    }
    started_ = true;
    style_ = session->layerStyle(layer);
    documentGlobalLight(*session->document(), globalAngle_, globalAltitude_);

    auto* outer = new QVBoxLayout(this);
    auto* row = new QHBoxLayout;
    list_ = new QListWidget(this);
    list_->setFixedWidth(fontMetrics().horizontalAdvance(QStringLiteral("Gradient Overlay")) + 64);
    pages_ = new QStackedWidget(this);
    row->addWidget(list_);
    row->addWidget(pages_, 1);
    outer->addLayout(row, 1);

    // Blending Options
    auto* blending = newPage(tr("Blending Options"));
    list_->addItem(tr("Blending Options"));
    checkRow(blending, tr("Show effects"), &style_.visible);
    checkRow(blending, tr("Layer mask hides effects"), &style_.maskHidesEffects);
    checkRow(blending, tr("Blend interior effects as group"), &style_.blendInteriorAsGroup);
    addEffectPages();

    connect(list_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    // Each effect's box switches it on and off; its settings stay while it is off.
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        const int row = list_->row(item);
        if (row <= 0) return;
        bool& on = enabled(Effect(row - 1));
        const bool checked = item->checkState() == Qt::Checked;
        if (on != checked) { on = checked; changed(); }
    });
    list_->setCurrentRow(std::clamp(page, 0, int(EffectCount)));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);
}

LayerStyleDialog::~LayerStyleDialog() {
    if (!finished_ && session_) session_->endLayerStyleEdit(false);
}

void LayerStyleDialog::accept() {
    if (!finished_ && session_) {
        // Nothing changed: the layer keeps the file's own bytes.
        session_->endLayerStyleEdit(touched_);
        finished_ = true;
    }
    QDialog::accept();
}

void LayerStyleDialog::reject() {
    if (!finished_ && session_) { session_->endLayerStyleEdit(false); finished_ = true; }
    QDialog::reject();
}

void LayerStyleDialog::changed() {
    touched_ = true;
    if (session_) session_->previewLayerStyle(output());
}

bool& LayerStyleDialog::enabled(Effect e) {
    switch (e) {
    case BevelEmboss: return style_.bevels.front().enabled;
    case Stroke: return style_.strokes.front().enabled;
    case InnerShadow: return style_.innerShadows.front().enabled;
    case InnerGlow: return style_.innerGlows.front().enabled;
    case Satin: return style_.satins.front().enabled;
    case ColorOverlay: return style_.colorOverlays.front().enabled;
    case GradientOverlay: return style_.gradientOverlays.front().enabled;
    case PatternOverlay: return style_.patternOverlays.front().enabled;
    case OuterGlow: return style_.outerGlows.front().enabled;
    case DropShadow: case EffectCount: break;
    }
    return style_.dropShadows.front().enabled;
}

LayerStyle LayerStyleDialog::output() const {
    LayerStyle s = style_;
    auto drop = [&](auto& list, Effect e) { if (madeUp_[e] && !list.empty() && !list.front().enabled) list.erase(list.begin()); };
    drop(s.bevels, BevelEmboss); drop(s.strokes, Stroke); drop(s.innerShadows, InnerShadow); drop(s.innerGlows, InnerGlow);
    drop(s.satins, Satin); drop(s.colorOverlays, ColorOverlay); drop(s.gradientOverlays, GradientOverlay);
    drop(s.patternOverlays, PatternOverlay); drop(s.outerGlows, OuterGlow); drop(s.dropShadows, DropShadow);
    return s;
}

// ---- Pages ---------------------------------------------------------------------------------------------------

void LayerStyleDialog::addEffectPages() {
    const QStringList techniques{tr("Softer"), tr("Precise")};
    for (int e = 0; e < EffectCount; e++) {
        const QString name = tr(kEffectNames[e]);
        QFormLayout* f = newPage(name);
        switch (Effect(e)) {
        case BevelEmboss: {
            auto* b = firstOf(style_.bevels, madeUp_[e]);
            comboRow(f, tr("Style"), {tr("Outer Bevel"), tr("Inner Bevel"), tr("Emboss"), tr("Pillow Emboss"), tr("Stroke Emboss")},
                     [b] { const Bevel::Kind order[] = {Bevel::Kind::Outer, Bevel::Kind::Inner, Bevel::Kind::Emboss, Bevel::Kind::Pillow, Bevel::Kind::StrokeEmboss};
                           return int(std::find(std::begin(order), std::end(order), b->kind) - std::begin(order)); },
                     [b](int i) { const Bevel::Kind order[] = {Bevel::Kind::Outer, Bevel::Kind::Inner, Bevel::Kind::Emboss, Bevel::Kind::Pillow, Bevel::Kind::StrokeEmboss};
                                  b->kind = order[std::clamp(i, 0, 4)]; });
            comboRow(f, tr("Technique"), {tr("Smooth"), tr("Chisel Hard"), tr("Chisel Soft")},
                     [b] { return int(b->technique); }, [b](int i) { b->technique = Bevel::Technique(i); });
            numberRow(f, tr("Depth"), &b->depth, 0.01, 10, QStringLiteral(" ×"), 2);
            comboRow(f, tr("Direction"), {tr("Up"), tr("Down")}, [b] { return b->up ? 0 : 1; }, [b](int i) { b->up = i == 0; });
            numberRow(f, tr("Size"), &b->size, 0, 250, QStringLiteral(" px"));
            numberRow(f, tr("Soften"), &b->soften, 0, 16, QStringLiteral(" px"));
            lightRows(f, &b->angle, &b->useGlobalLight, &b->altitude);
            blendRow(f, tr("Highlight mode"), &b->highlightMode);
            colourRow(f, tr("Highlight colour"), &b->highlight);
            percentRow(f, tr("Highlight opacity"), &b->highlightOpacity);
            blendRow(f, tr("Shadow mode"), &b->shadowMode);
            colourRow(f, tr("Shadow colour"), &b->shadow);
            percentRow(f, tr("Shadow opacity"), &b->shadowOpacity);
            checkRow(f, tr("Contour"), &b->useContour);
            percentRow(f, tr("Contour range"), &b->contourRange);
            if (!b->texturePattern.empty()) {   // a texture needs a pattern the document has
                checkRow(f, tr("Texture"), &b->useTexture);
                percentRow(f, tr("Texture scale"), &b->textureScale, 1000);
                percentRow(f, tr("Texture depth"), &b->textureDepth, 1000);
                checkRow(f, tr("Invert texture"), &b->textureInvert);
            }
            break;
        }
        case Stroke: {
            auto* s = firstOf(style_.strokes, madeUp_[e]);
            numberRow(f, tr("Size"), &s->size, 1, 250, QStringLiteral(" px"));
            comboRow(f, tr("Position"), {tr("Outside"), tr("Inside"), tr("Center")},
                     [s] { return int(s->position); }, [s](int i) { s->position = compositor::Stroke::Position(i); });
            blendRow(f, tr("Blend mode"), &s->mode);
            percentRow(f, tr("Opacity"), &s->opacity);
            checkRow(f, tr("Overprint"), &s->overprint);
            comboRow(f, tr("Fill type"), {tr("Color"), tr("Gradient")}, [s] { return s->gradientFill ? 1 : 0; }, [s](int i) { s->gradientFill = i == 1; });
            colourRow(f, tr("Colour"), &s->color);
            gradientRows(f, &s->gradient);
            break;
        }
        case InnerShadow: {
            auto* s = firstOf(style_.innerShadows, madeUp_[e]);
            blendRow(f, tr("Blend mode"), &s->mode);
            colourRow(f, tr("Colour"), &s->color);
            percentRow(f, tr("Opacity"), &s->opacity);
            lightRows(f, &s->angle, &s->useGlobalLight);
            numberRow(f, tr("Distance"), &s->distance, 0, 30000, QStringLiteral(" px"));
            numberRow(f, tr("Choke"), &s->choke, 0, 100, QStringLiteral(" %"));
            numberRow(f, tr("Size"), &s->size, 0, 250, QStringLiteral(" px"));
            break;
        }
        case InnerGlow: {
            auto* g = firstOf(style_.innerGlows, madeUp_[e]);
            blendRow(f, tr("Blend mode"), &g->mode);
            percentRow(f, tr("Opacity"), &g->opacity);
            colourRow(f, tr("Colour"), &g->color);
            comboRow(f, tr("Technique"), techniques, [g] { return g->precise ? 1 : 0; }, [g](int i) { g->precise = i == 1; });
            comboRow(f, tr("Source"), {tr("Edge"), tr("Center")}, [g] { return g->center ? 1 : 0; }, [g](int i) { g->center = i == 1; });
            numberRow(f, tr("Choke"), &g->choke, 0, 100, QStringLiteral(" %"));
            numberRow(f, tr("Size"), &g->size, 0, 250, QStringLiteral(" px"));
            numberRow(f, tr("Range"), &g->range, 1, 100, QStringLiteral(" %"));
            break;
        }
        case Satin: {
            auto* s = firstOf(style_.satins, madeUp_[e]);
            blendRow(f, tr("Blend mode"), &s->mode);
            colourRow(f, tr("Colour"), &s->color);
            percentRow(f, tr("Opacity"), &s->opacity);
            numberRow(f, tr("Angle"), &s->angle, -180, 180, QStringLiteral("°"));
            numberRow(f, tr("Distance"), &s->distance, 1, 250, QStringLiteral(" px"));
            numberRow(f, tr("Size"), &s->size, 0, 250, QStringLiteral(" px"));
            checkRow(f, tr("Invert"), &s->invert);
            break;
        }
        case ColorOverlay: {
            auto* c = firstOf(style_.colorOverlays, madeUp_[e]);
            blendRow(f, tr("Blend mode"), &c->mode);
            colourRow(f, tr("Colour"), &c->color);
            percentRow(f, tr("Opacity"), &c->opacity);
            break;
        }
        case GradientOverlay: {
            auto* g = firstOf(style_.gradientOverlays, madeUp_[e]);
            blendRow(f, tr("Blend mode"), &g->mode);
            percentRow(f, tr("Opacity"), &g->opacity);
            gradientRows(f, &g->gradient);
            break;
        }
        case PatternOverlay: {
            auto* p = firstOf(style_.patternOverlays, madeUp_[e]);
            blendRow(f, tr("Blend mode"), &p->mode);
            percentRow(f, tr("Opacity"), &p->opacity);
            // The document's patterns (by id: a PSD's blocks carry no names we keep), then the imported ones it
            // does not have yet (by name), which join the document when chosen.
            auto patterns = session_ && session_->document() ? documentPatterns(*session_->document()) : nullptr;
            QStringList ids, labels;
            if (patterns) for (auto& [id, tile] : *patterns) { ids << QString::fromStdString(id); labels << ids.back(); }
            for (const auto& preset : PresetLibrary::instance().patterns()) {
                const QString id = QString::fromStdString(preset.id);
                if (ids.contains(id)) continue;
                ids << id;
                labels << tr("%1 (imported)").arg(preset.name.empty() ? id : QString::fromStdString(preset.name));
            }
            if (ids.isEmpty()) {
                f->addRow(new QLabel(tr("This document has no patterns. File ▸ Import Presets… adds Photoshop .pat patterns."), this));
                if (p->patternId.empty()) { list_->addItem(name); list_->item(list_->count() - 1)->setFlags(Qt::NoItemFlags); continue; }
            } else {
                if (p->patternId.empty()) p->patternId = ids.front().toStdString();
                comboRow(f, tr("Pattern"), labels, [p, ids] { return std::max(0, int(ids.indexOf(QString::fromStdString(p->patternId)))); },
                         [p, ids](int i) { p->patternId = ids.value(i).toStdString(); });
            }
            numberRow(f, tr("Angle"), &p->angle, -180, 180, QStringLiteral("°"));
            percentRow(f, tr("Scale"), &p->scale, 1000);
            checkRow(f, tr("Link with layer"), &p->linkWithLayer);
            break;
        }
        case OuterGlow: {
            auto* g = firstOf(style_.outerGlows, madeUp_[e]);
            blendRow(f, tr("Blend mode"), &g->mode);
            percentRow(f, tr("Opacity"), &g->opacity);
            colourRow(f, tr("Colour"), &g->color);
            comboRow(f, tr("Technique"), techniques, [g] { return g->precise ? 1 : 0; }, [g](int i) { g->precise = i == 1; });
            numberRow(f, tr("Spread"), &g->spread, 0, 100, QStringLiteral(" %"));
            numberRow(f, tr("Size"), &g->size, 0, 250, QStringLiteral(" px"));
            numberRow(f, tr("Range"), &g->range, 1, 100, QStringLiteral(" %"));
            break;
        }
        case DropShadow: {
            auto* s = firstOf(style_.dropShadows, madeUp_[e]);
            blendRow(f, tr("Blend mode"), &s->mode);
            colourRow(f, tr("Colour"), &s->color);
            percentRow(f, tr("Opacity"), &s->opacity);
            lightRows(f, &s->angle, &s->useGlobalLight);
            numberRow(f, tr("Distance"), &s->distance, 0, 30000, QStringLiteral(" px"));
            numberRow(f, tr("Spread"), &s->spread, 0, 100, QStringLiteral(" %"));
            numberRow(f, tr("Size"), &s->size, 0, 250, QStringLiteral(" px"));
            checkRow(f, tr("Layer knocks out drop shadow"), &s->layerConceals);
            break;
        }
        case EffectCount: break;
        }
        auto* item = new QListWidgetItem(name);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(enabled(Effect(e)) ? Qt::Checked : Qt::Unchecked);
        list_->addItem(item);
    }
}

QFormLayout* LayerStyleDialog::newPage(const QString& title) {
    auto* page = new QWidget(pages_);
    auto* box = new QVBoxLayout(page);
    auto* heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()), page);
    box->addWidget(heading);
    auto* form = new QFormLayout;
    box->addLayout(form);
    box->addStretch(1);
    pages_->addWidget(page);
    return form;
}

void LayerStyleDialog::blendRow(QFormLayout* form, const QString& label, EffectBlend* mode) {
    QStringList names;
    for (int i = 0; i < kBlendCount; i++) names << tr(kBlendNames[i]);
    comboRow(form, label, names, [mode] { return int(*mode); }, [mode](int i) { *mode = EffectBlend(std::clamp(i, 0, kBlendCount - 1)); });
}

void LayerStyleDialog::colourRow(QFormLayout* form, const QString& label, StyleColor* colour) {
    auto* button = new QPushButton(this);
    paintSwatch(button, *colour);
    connect(button, &QPushButton::clicked, this, [this, button, colour, label] {
        const QColor chosen = QColorDialog::getColor(QColor(colour->r, colour->g, colour->b), this, label);
        if (!chosen.isValid()) return;
        *colour = {uint8_t(chosen.red()), uint8_t(chosen.green()), uint8_t(chosen.blue())};
        paintSwatch(button, *colour);
        changed();
    });
    form->addRow(label, button);
}

void LayerStyleDialog::percentRow(QFormLayout* form, const QString& label, float* fraction, double max) {
    auto* spin = new QDoubleSpinBox(this);
    spin->setRange(0, max);
    spin->setDecimals(0);
    spin->setSuffix(QStringLiteral(" %"));
    spin->setValue(double(*fraction) * 100);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, fraction](double v) { *fraction = float(v / 100); changed(); });
    form->addRow(label, spin);
}

void LayerStyleDialog::numberRow(QFormLayout* form, const QString& label, float* value, double min, double max, const QString& suffix, int decimals) {
    auto* spin = new QDoubleSpinBox(this);
    spin->setRange(min, max);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setValue(double(*value));
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, value](double v) { *value = float(v); changed(); });
    form->addRow(label, spin);
}

void LayerStyleDialog::checkRow(QFormLayout* form, const QString& label, bool* value) {
    auto* box = new QCheckBox(label, this);
    box->setChecked(*value);
    connect(box, &QCheckBox::toggled, this, [this, value](bool on) { *value = on; changed(); });
    form->addRow(box);
}

void LayerStyleDialog::comboRow(QFormLayout* form, const QString& label, const QStringList& items, std::function<int()> get, std::function<void(int)> set) {
    auto* combo = new QComboBox(this);
    combo->addItems(items);
    combo->setCurrentIndex(std::clamp(get(), 0, int(items.size()) - 1));
    connect(combo, &QComboBox::currentIndexChanged, this, [this, set](int i) { if (i >= 0) { set(i); changed(); } });
    form->addRow(label, combo);
}

void LayerStyleDialog::lightRows(QFormLayout* form, float* angle, bool* global, float* altitude) {
    // With Use Global Light the effect follows the document's light, as Photoshop draws it.
    auto* angleSpin = new QDoubleSpinBox(this);
    angleSpin->setRange(-180, 180);
    angleSpin->setDecimals(0);
    angleSpin->setSuffix(QStringLiteral("°"));
    angleSpin->setValue(double(*angle));
    angleSpin->setEnabled(!*global);
    connect(angleSpin, &QDoubleSpinBox::valueChanged, this, [this, angle](double v) { *angle = float(v); changed(); });
    form->addRow(tr("Angle"), angleSpin);
    QDoubleSpinBox* altitudeSpin = nullptr;
    if (altitude) {
        altitudeSpin = new QDoubleSpinBox(this);
        altitudeSpin->setRange(0, 90);
        altitudeSpin->setDecimals(0);
        altitudeSpin->setSuffix(QStringLiteral("°"));
        altitudeSpin->setValue(double(*altitude));
        altitudeSpin->setEnabled(!*global);
        connect(altitudeSpin, &QDoubleSpinBox::valueChanged, this, [this, altitude](double v) { *altitude = float(v); changed(); });
        form->addRow(tr("Altitude"), altitudeSpin);
    }
    auto* box = new QCheckBox(tr("Use Global Light"), this);
    box->setChecked(*global);
    connect(box, &QCheckBox::toggled, this, [this, angle, global, altitude, angleSpin, altitudeSpin](bool on) {
        *global = on;
        if (on) {
            *angle = globalAngle_;
            QSignalBlocker block(angleSpin);
            angleSpin->setValue(double(globalAngle_));
            if (altitude) { *altitude = globalAltitude_; QSignalBlocker b2(altitudeSpin); altitudeSpin->setValue(double(globalAltitude_)); }
        }
        angleSpin->setEnabled(!on);
        if (altitudeSpin) altitudeSpin->setEnabled(!on);
        changed();
    });
    form->addRow(box);
}

void LayerStyleDialog::gradientRows(QFormLayout* form, StyleGradient* g) {
    // The ends of the gradient; stops between them are kept as they are.
    if (g->colors.empty()) g->colors = {{0, {0, 0, 0}, 0.5f}, {1, {255, 255, 255}, 0.5f}};
    if (g->colors.size() == 1) g->colors.push_back({1, g->colors.front().color, 0.5f});
    if (g->alphas.empty()) g->alphas = {{0, 1, 0.5f}, {1, 1, 0.5f}};
    // Imported gradients (PresetLibrary) replace the stops; their foreground and background stops take the colours set now.
    const auto& presets = PresetLibrary::instance().gradients();
    QComboBox* presetCombo = nullptr;
    if (!presets.empty()) {
        presetCombo = new QComboBox(this);
        presetCombo->addItem(tr("Custom"));
        for (const auto& p : presets) presetCombo->addItem(QString::fromStdString(p.name));
        form->addRow(tr("Preset"), presetCombo);
    }
    // The colour rows edit the first and last stops, looked up when used: a preset replaces the stop list.
    auto endRow = [this, form, g](const QString& label, bool first) {
        auto* button = new QPushButton(this);
        paintSwatch(button, first ? g->colors.front().color : g->colors.back().color);
        connect(button, &QPushButton::clicked, this, [this, button, g, first, label] {
            StyleColor& colour = first ? g->colors.front().color : g->colors.back().color;
            const QColor chosen = QColorDialog::getColor(QColor(colour.r, colour.g, colour.b), this, label);
            if (!chosen.isValid()) return;
            StyleColor& target = first ? g->colors.front().color : g->colors.back().color;
            target = {uint8_t(chosen.red()), uint8_t(chosen.green()), uint8_t(chosen.blue())};
            paintSwatch(button, target);
            changed();
        });
        form->addRow(label, button);
        return button;
    };
    auto* startButton = endRow(tr("Start colour"), true);
    auto* endButton = endRow(tr("End colour"), false);
    if (presetCombo)
        connect(presetCombo, &QComboBox::currentIndexChanged, this, [this, g, startButton, endButton](int i) {
            const auto& list = PresetLibrary::instance().gradients();
            if (i < 1 || i > int(list.size())) return;
            QColor fg = session_ ? session_->foregroundColor : QColor(Qt::black), bg = session_ ? session_->backgroundColor : QColor(Qt::white);
            const StyleGradient chosen = list[size_t(i - 1)].styleGradient({uint8_t(fg.red()), uint8_t(fg.green()), uint8_t(fg.blue())},
                                                                          {uint8_t(bg.red()), uint8_t(bg.green()), uint8_t(bg.blue())});
            const StyleColor front = chosen.colors.front().color, back = chosen.colors.back().color;
            g->colors = chosen.colors;
            g->alphas = chosen.alphas;
            g->smoothness = chosen.smoothness;
            paintSwatch(startButton, front);
            paintSwatch(endButton, back);
            changed();
        });
    comboRow(form, tr("Style"), {tr("Linear"), tr("Radial"), tr("Angle"), tr("Reflected"), tr("Diamond"), tr("Shape Burst")},
             [g] { return int(g->type); }, [g](int i) { g->type = StyleGradient::Type(i); });
    numberRow(form, tr("Angle"), &g->angle, -180, 180, QStringLiteral("°"));
    percentRow(form, tr("Scale"), &g->scale, 150);
    checkRow(form, tr("Reverse"), &g->reverse);
    checkRow(form, tr("Dither"), &g->dither);
    checkRow(form, tr("Align with layer"), &g->alignWithLayer);
}

} // namespace app
