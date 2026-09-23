// Modeless dialogs for the destructive adjustments (Image > Adjustments) and
// filters (Filter menu), previewing on the canvas and committing on OK.
#pragma once
#include "AdjustmentEditor.h"
#include "PixelDialog.h"
#include "compositor/subject.h"
#include <thread>

class QCheckBox;

namespace app {

/// Image > Adjustments > Levels / Curves / Hue-Saturation / Exposure / Gradient Map / Grain on pixels.
class PixelAdjustmentDialog : public PixelDialog {
    Q_OBJECT
public:
    PixelAdjustmentDialog(EditorSession* session, compositor::AdjustmentKind kind, QWidget* parent = nullptr);
protected:
    bool apply() override;
private:
    void refreshPreview();
    std::shared_ptr<compositor::Image> run(const compositor::Image& source, double scale) const;
    AdjustmentEditor* editor_;
};

/// Filter > Gaussian Blur / Motion Blur / Add Noise / Lens Correction.
class FilterDialog : public PixelDialog {
    Q_OBJECT
public:
    FilterDialog(EditorSession* session, compositor::FilterKind kind, QWidget* parent = nullptr);
protected:
    bool apply() override;
private:
    void prepareSource();
    void refreshPreview();
    bool identity() const;
    std::shared_ptr<compositor::Image> run(const compositor::Image& source, double scale) const;
    compositor::FilterKind kind_;
    compositor::FilterSettings settings_;
    uint32_t seed_;
    int margin_ = -1;
    std::vector<std::function<void()>> syncers_;
};

/// Filter > Remove Background: the subject mask from the model, refined live, applied as a layer mask.
class BackgroundDialog : public PixelDialog {
    Q_OBJECT
public:
    /// `quickModelPath`, when set, is a fast coarse model run first for an instant preview.
    BackgroundDialog(EditorSession* session, QString modelPath, QString quickModelPath = {}, QWidget* parent = nullptr);
    ~BackgroundDialog() override;
protected:
    bool apply() override;
private:
    void refreshPreview();
    void startDetail();
    std::shared_ptr<compositor::GrayImage> refined(int limit) const;
    QString modelPath_;
    QWidget* advanced_;
    bool advancedMode_ = false;
    compositor::MatteSettings settings_;
    std::shared_ptr<compositor::GrayImage> raw_;        // the mask the sliders refine: coarse or detailed
    std::shared_ptr<compositor::GrayImage> coarse_, detailed_;
    bool detail_ = false;
    QString error_;
    bool computing_ = false;
    std::thread worker_;   // runs the model; joined before the dialog goes, so a quit mid-run waits for it
};

} // namespace app
