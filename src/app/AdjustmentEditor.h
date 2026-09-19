// Controls for one adjustment's settings: Levels, Curves, Hue/Saturation,
// Exposure, Gradient Map or Grain. Used by the Adjustments panel for
// adjustment layers and by the Image > Adjustments dialogs for pixels.
#pragma once
#include "compositor/adjustments.h"
#include <QWidget>
#include <optional>
#include <array>
#include <vector>

class QComboBox;
class QSlider;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;

namespace app {

class HistogramWidget;
class CurveWidget;

class AdjustmentEditor : public QWidget {
    Q_OBJECT
public:
    explicit AdjustmentEditor(QWidget* parent = nullptr);
    void setSettings(const compositor::AdjustmentSettings& settings);
    const compositor::AdjustmentSettings& settings() const { return settings_; }
    void setHistogram(const std::array<std::vector<double>, 4>& histogram);
    /// Lets the Hue/Saturation eyedroppers and the targeted-adjustment drag sample the canvas.
    void setSession(class EditorSession* session);
    ~AdjustmentEditor() override;

signals:
    /// A slider drag began / a value changed / the drag ended.
    void editStarted();
    void settingsChanged(const compositor::AdjustmentSettings& settings);
    void editFinished();

private:
    void rebuild();
    void sync();
    QWidget* buildLevels();
    QWidget* buildCurves();
    QWidget* buildHsv();
    QWidget* buildExposure();
    QWidget* buildGradientMap();
    QWidget* buildGrain();
    void changed();
    /// A slider paired with a spin box, both writing to `apply`.
    QWidget* sliderRow(const QString& label, double min, double max, int decimals, double scale, std::function<double()> get, std::function<void(double)> apply);

    compositor::AdjustmentSettings settings_;
    QWidget* body_ = nullptr;
    bool syncing_ = false;
    std::vector<std::function<void()>> syncers_;
    HistogramWidget* histogram_ = nullptr;
    CurveWidget* curve_ = nullptr;
    std::array<std::vector<double>, 4> histogramData_;
    EditorSession* session_ = nullptr;
    int hueSampleMode_ = 0; // 0 off, 1 sample, 2 add, 3 remove
    bool hueTargeting_ = false;
    void installHueHooks();
    void installLevelsHook();
    std::optional<compositor::LevelsSample> levelsSample_;
    void clearHueHooks();
    struct HueDrag { int range; double hue, saturation; };
    std::optional<HueDrag> hueDrag_;
};

} // namespace app
