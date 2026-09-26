// Filter > Camera Raw Filter: the grade of upstream Compositor's Camera Raw panels (UI/CameraRaw*.swift,
// RawDevelopSheet.swift) as a modeless pixel dialog with a live preview; OK bakes it into the layer.
#pragma once
#include "PixelDialog.h"
#include "compositor/cameraraw.h"
#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QTimer;
class QWidget;

namespace app {

class CameraRawDialog : public PixelDialog {
    Q_OBJECT
public:
    CameraRawDialog(EditorSession* session, QWidget* parent = nullptr);
protected:
    bool apply() override;
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
    void settingsChanged();
    void refreshPreview();
    std::shared_ptr<compositor::Image> run(const compositor::Image& source, double scale, const compositor::CameraRawPreview& preview) const;

    compositor::CameraRawSettings settings_;
    compositor::CameraRawPreview preview_;
    uint32_t seed_;
    int mixerChannel_ = 0;   // 0 hue, 1 saturation, 2 luminance
    int gradeWheel_ = 0;     // 0 shadows, 1 midtones, 2 highlights, 3 global
    bool syncing_ = false;
    std::vector<std::function<void()>> syncers_;
    QTimer* debounce_ = nullptr;
    QComboBox* whiteBalance_ = nullptr;
};

} // namespace app
