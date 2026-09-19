// Controls for one adjustment's settings: Levels, Curves, Hue/Saturation,
// Exposure, Gradient Map or Grain. Used by the Adjustments panel for
// adjustment layers and by the Image > Adjustments dialogs for pixels.
#pragma once
#include "compositor/adjustments.h"
#include <QWidget>
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
};

} // namespace app
