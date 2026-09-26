// Layer ▸ Layer Style: Photoshop's Layer Style dialog. Blending Options and the ten effects in its list, each with its
// switch; the page on the right edits the effect's first instance (further ones are kept as they are). Every change
// shows on the layer at once; Cancel puts the layer back.
#pragma once
#include "EditorSession.h"
#include "compositor/layerstyle.h"
#include <QDialog>
#include <QPointer>
#include <array>
#include <functional>

class QFormLayout;
class QListWidget;
class QStackedWidget;

namespace app {

class LayerStyleDialog : public QDialog {
    Q_OBJECT
public:
    /// `page`: the effect to open on (0 Blending Options, then the list's order).
    LayerStyleDialog(EditorSession* session, const compositor::Uuid& layer, QWidget* parent = nullptr, int page = 0);
    ~LayerStyleDialog() override;
    /// Whether the dialog could start (the layer takes a style).
    bool started() const { return started_; }
    /// The effects in the dialog's list, in Photoshop's order.
    enum Effect { BevelEmboss, Stroke, InnerShadow, InnerGlow, Satin, ColorOverlay, GradientOverlay, PatternOverlay, OuterGlow, DropShadow, EffectCount };

protected:
    void accept() override;
    void reject() override;

private:
    void changed();
    /// The style as it stands, without the effects the dialog made up and left off.
    compositor::LayerStyle output() const;
    bool& enabled(Effect e);
    void addEffectPages();

    // Row builders for the pages, bound to fields of `style_`.
    QFormLayout* newPage(const QString& title);
    void blendRow(QFormLayout* form, const QString& label, compositor::EffectBlend* mode);
    void colourRow(QFormLayout* form, const QString& label, compositor::StyleColor* colour);
    void percentRow(QFormLayout* form, const QString& label, float* fraction, double max = 100);
    void numberRow(QFormLayout* form, const QString& label, float* value, double min, double max, const QString& suffix, int decimals = 0);
    void checkRow(QFormLayout* form, const QString& label, bool* value);
    void comboRow(QFormLayout* form, const QString& label, const QStringList& items, std::function<int()> get, std::function<void(int)> set);
    void lightRows(QFormLayout* form, float* angle, bool* global, float* altitude = nullptr);
    void gradientRows(QFormLayout* form, compositor::StyleGradient* gradient);

    QPointer<EditorSession> session_;
    compositor::Uuid layer_;
    compositor::LayerStyle style_;
    std::array<bool, EffectCount> madeUp_{};   // the dialog added this effect's instance; dropped again while off
    float globalAngle_ = 120, globalAltitude_ = 30;
    bool started_ = false, touched_ = false, finished_ = false;
    QListWidget* list_ = nullptr;
    QStackedWidget* pages_ = nullptr;
};

} // namespace app
