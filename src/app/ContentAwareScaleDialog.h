// Edit > Content-Aware Scale: the active layer's new width and height in percent, Protect (the selection's
// pixels are kept), and a live preview carved from a reduced copy of the layer.
#pragma once
#include <QDialog>
#include <QTimer>
#include <memory>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;

namespace compositor { class Image; class GrayImage; }

namespace app {

class EditorSession;

class ContentAwareScaleDialog : public QDialog {
    Q_OBJECT
public:
    ContentAwareScaleDialog(EditorSession* session, QWidget* parent = nullptr);
    ~ContentAwareScaleDialog() override;

private:
    void updatePreview();
    void apply();

    EditorSession* session_;
    QDoubleSpinBox* width_ = nullptr;
    QDoubleSpinBox* height_ = nullptr;
    QCheckBox* protect_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* size_ = nullptr;
    QTimer timer_;
    std::shared_ptr<compositor::Image> thumb_;
    std::shared_ptr<compositor::GrayImage> thumbProtect_;
    int pixelWidth_ = 0, pixelHeight_ = 0;
};

} // namespace app
