// Controls for one adjustment's settings: Levels, Curves, Hue/Saturation,
// Exposure, Gradient Map or Grain. Used by the Adjustments panel for
// adjustment layers and by the Image > Adjustments dialogs for pixels.
#pragma once
#include "compositor/adjustments.h"
#include "compositor/colormgmt.h"
#include "compositor/colorprofile.h"
#include "compositor/imaget.h"
#include <QPointer>
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

class EditorSession;
class HistogramView;
class CurveWidget;

class AdjustmentEditor : public QWidget {
    Q_OBJECT
public:
    explicit AdjustmentEditor(QWidget* parent = nullptr);
    void setSettings(const compositor::AdjustmentSettings& settings);
    const compositor::AdjustmentSettings& settings() const { return settings_; }
    void setHistogram(const std::array<std::vector<double>, 4>& histogram);
    /// Composite and four channels (CMYK's black in the last).
    void setHistogram(const std::array<std::vector<double>, 5>& histogram);
    /// Lets the Hue/Saturation eyedroppers and the targeted-adjustment drag sample the canvas.
    void setSession(class EditorSession* session);
    ~AdjustmentEditor() override;
    /// What the Levels and Curves clipping display adjusts (Alt on the black or white point, or Curves' Show Clipping):
    /// the pixels the adjustment takes, reduced, and where they lie on the document. Asked for when the display starts.
    struct ClippingSource {
        compositor::AnyImage image;
        compositor::Affine pixelToDocument;
        compositor::ColorMode mode = compositor::ColorMode::RGB;
        compositor::ColorProfile profile;
        compositor::TransferCurve curve = compositor::TransferCurve::srgb();
    };
    void setClippingSource(std::function<std::optional<ClippingSource>()> source) { clippingSource_ = std::move(source); }
    /// The longest side the clipping display works at.
    static constexpr int clippingLimit = 2048;
    /// Shows the clipping display for the black (1) or white (2) point, or hides it (0); never a document edit.
    void showClipping(int point);

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
    /// Photoshop's other adjustment layers: Invert, Brightness/Contrast, Posterize, Threshold, Black & White, Color
    /// Balance, Vibrance, Photo Filter, Channel Mixer, Selective Color.
    QWidget* buildMore();
    QWidget* checkRow(const QString& label, std::function<bool()> get, std::function<void(bool)> apply);
    QWidget* colourRow(const QString& label, std::function<compositor::AdjustmentColor&()> colour);
    void changed();
    /// A slider paired with a spin box, both writing to `apply`.
    /// `clipPoint` 1 or 2: the Levels black or white input, whose drag with Alt held shows the clipping display.
    QWidget* sliderRow(const QString& label, double min, double max, int decimals, double scale, std::function<double()> get, std::function<void(double)> apply, int clipPoint = 0);
    void refreshClipping();
    std::function<std::optional<ClippingSource>()> clippingSource_;
    std::optional<ClippingSource> clipping_;
    int clipPoint_ = 0;
    bool curvesShowClipping_ = false;

    compositor::AdjustmentSettings settings_;
    int toneRange_ = 1, mixerOutput_ = 0, selectiveRange_ = 0;   // which part of a many-part adjustment is shown
    QWidget* body_ = nullptr;
    bool syncing_ = false;
    std::vector<std::function<void()>> syncers_;
    HistogramView* histogram_ = nullptr;
    CurveWidget* curve_ = nullptr;
    std::array<std::vector<double>, 5> histogramData_;
    /// The session's document mode (RGB without one): the channel names and what the editors offer.
    compositor::ColorMode colorMode() const;
    int shownChannel(int channel) const;
    compositor::ColorMode builtMode_ = compositor::ColorMode::RGB;   // the mode the editors were built for
    QComboBox* channelCombo();
    QPointer<EditorSession> session_;   // may go first: its tab can close while a dialog holding this editor is open
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
