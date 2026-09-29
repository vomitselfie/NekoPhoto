// Filter > Camera Raw Filter: the Camera Raw panels (CameraRawPanels.h) as a modeless pixel dialog with a live
// preview; OK bakes the grade into the layer.
#pragma once
#include "PixelDialog.h"
#include "compositor/cameraraw.h"

class QTimer;

namespace app {

class CameraRawPanels;

class CameraRawDialog : public PixelDialog {
    Q_OBJECT
public:
    CameraRawDialog(EditorSession* session, QWidget* parent = nullptr);
protected:
    bool apply() override;
private:
    void refreshPreview();
    std::shared_ptr<compositor::Image> run(const compositor::Image& source, double scale, const compositor::CameraRawPreview& preview) const;
    std::shared_ptr<compositor::Image16> run(const compositor::Image16& source, double scale, const compositor::CameraRawPreview& preview) const;

    CameraRawPanels* panels_ = nullptr;
    uint32_t seed_;
    QTimer* debounce_ = nullptr;
};

} // namespace app
