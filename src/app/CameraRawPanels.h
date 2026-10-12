// The Camera Raw panels (Basic, Curve, Detail, Color, Optics, Geometry, Effects, Calibration) of upstream Compositor's
// UI/CameraRaw*.swift and RawDevelopSheet.swift, bound to one CameraRawSettings. Filter > Camera Raw Filter and the
// Camera Raw dialog a RAW file opens in (RawDevelopDialog.h) both show them.
#pragma once
#include "nekophoto/cameraraw.h"
#include "nekophoto/raw.h"
#include <QWidget>
#include <array>
#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;

namespace app {

class CameraRawPanels : public QWidget {
    Q_OBJECT
public:
    /// Filter: White Balance is Custom or Auto, relative. Raw: As Shot (the file's own balance), Auto, the presets the file
    /// records and Custom, with Temperature in kelvin and Tint when `raw` has a colour model (relative otherwise).
    enum class Mode { Filter, Raw };
    CameraRawPanels(Mode mode, const nekophoto::CameraRawSettings& settings, QWidget* parent = nullptr,
                    const nekophoto::RawWhiteBalance* raw = nullptr);

    const nekophoto::CameraRawSettings& settings() const { return settings_; }
    void setSettings(const nekophoto::CameraRawSettings& settings);
    /// The preview-only overlays the panels' check boxes turn on (clipping, the sharpening mask).
    const nekophoto::CameraRawPreview& preview() const { return preview_; }
    /// White Balance > Auto: the host's gray-world balance of the image it grades, as relative Temperature and Tint, or
    /// for a RAW file with a colour model as kelvin and Tint; empty when there is none yet.
    std::function<std::optional<std::array<double, 2>>()> autoBalance;

signals:
    /// A setting or overlay changed (not debounced).
    void changed();

private:
    QWidget* basicPage();
    QWidget* curvePage();
    QWidget* detailPage();
    QWidget* colorPage();
    QWidget* opticsPage();
    QWidget* geometryPage();
    QWidget* effectsPage();
    QWidget* calibrationPage();
    /// A labelled slider and number bound to `value` in `form`; `step` is the slider's resolution.
    void slider(QFormLayout* form, const QString& label, double min, double max, double step, std::function<double&()> value,
                std::function<void()> changed = {});
    QCheckBox* check(QFormLayout* form, const QString& label, std::function<bool&()> value);
    /// A combo box whose index is bound through `get` / `set`.
    QComboBox* choice(QFormLayout* form, const QString& label, const QStringList& items, std::function<int()> get, std::function<void(int)> set);
    void sync();
    void emitChanged();
    /// Load Preset…: a Camera Raw or Lightroom .xmp preset over the current settings (nekophoto/camerarawxmp.h); what has
    /// no counterpart here is listed.
    void loadPreset();
    /// Reset All's settings: the defaults, As Shot for a RAW file.
    nekophoto::CameraRawSettings defaults() const;
    /// White Balance in kelvin for a RAW file (Mode::Raw with a colour model).
    void whiteBalanceKelvin(QFormLayout* white);
    static QString whiteBalanceName(nekophoto::CameraRawWhiteBalance mode);

    nekophoto::CameraRawSettings settings_;
    nekophoto::CameraRawPreview preview_;
    Mode mode_;
    int mixerChannel_ = 0;   // 0 hue, 1 saturation, 2 luminance
    int gradeWheel_ = 0;     // 0 shadows, 1 midtones, 2 highlights, 3 global
    bool syncing_ = false;
    std::vector<std::function<void()>> syncers_;
    QComboBox* whiteBalance_ = nullptr;
    std::optional<nekophoto::RawWhiteBalance> raw_;
};

} // namespace app
