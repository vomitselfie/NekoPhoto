// The Camera Raw panels (Basic, Curve, Detail, Color, Optics, Geometry, Effects, Calibration) of upstream Compositor's
// UI/CameraRaw*.swift and RawDevelopSheet.swift, bound to one CameraRawSettings. Filter > Camera Raw Filter and the
// Camera Raw dialog a RAW file opens in (RawDevelopDialog.h) both show them.
#pragma once
#include "compositor/cameraraw.h"
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
    /// Filter: White Balance is Custom or Auto. Raw: As Shot (the file's own balance), Auto or Custom.
    enum class Mode { Filter, Raw };
    CameraRawPanels(Mode mode, const compositor::CameraRawSettings& settings, QWidget* parent = nullptr);

    const compositor::CameraRawSettings& settings() const { return settings_; }
    void setSettings(const compositor::CameraRawSettings& settings);
    /// The preview-only overlays the panels' check boxes turn on (clipping, the sharpening mask).
    const compositor::CameraRawPreview& preview() const { return preview_; }
    /// White Balance > Auto: the host's gray-world balance of the image it grades; empty when there is none yet.
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

    compositor::CameraRawSettings settings_;
    compositor::CameraRawPreview preview_;
    Mode mode_;
    int mixerChannel_ = 0;   // 0 hue, 1 saturation, 2 luminance
    int gradeWheel_ = 0;     // 0 shadows, 1 midtones, 2 highlights, 3 global
    bool syncing_ = false;
    std::vector<std::function<void()>> syncers_;
    QComboBox* whiteBalance_ = nullptr;
};

} // namespace app
