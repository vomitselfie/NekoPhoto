// Modeless dialogs for the destructive adjustments (Image > Adjustments) and
// filters (Filter menu), previewing on the canvas and committing on OK.
#pragma once
#include "AdjustmentEditor.h"
#include "EditorSession.h"
#include "compositor/subject.h"
#include <QDialog>
#include <QPointer>
#include <thread>

class QCheckBox;

namespace app {

/// Image > Adjustments > Levels / Curves / Hue-Saturation / Exposure / Gradient Map / Grain on pixels.
class PixelAdjustmentDialog : public QDialog {
    Q_OBJECT
public:
    PixelAdjustmentDialog(EditorSession* session, compositor::AdjustmentKind kind, QWidget* parent = nullptr);
    ~PixelAdjustmentDialog() override;
protected:
    void done(int result) override;
private:
    void refreshPreview();
    std::shared_ptr<compositor::Image> run(const compositor::Image& source, double scale) const;
    QPointer<EditorSession> session_;   // the window destroys its sessions before Qt deletes child dialogs
    AdjustmentEditor* editor_;
    QCheckBox* preview_;
    std::shared_ptr<const compositor::Image> source_;
    compositor::LayerTransform transform_;
    std::shared_ptr<const compositor::Image> previewSource_;
    double previewScale_ = 1;
    std::shared_ptr<compositor::GrayImage> coverage_;
    std::shared_ptr<compositor::GrayImage> previewCoverage_;
    bool finished_ = false;
};

/// Filter > Gaussian Blur / Motion Blur / Add Noise / Lens Correction.
class FilterDialog : public QDialog {
    Q_OBJECT
public:
    FilterDialog(EditorSession* session, compositor::FilterKind kind, QWidget* parent = nullptr);
    ~FilterDialog() override;
protected:
    void done(int result) override;
private:
    void prepareSource();
    void refreshPreview();
    std::shared_ptr<compositor::Image> run(const compositor::Image& source, double scale) const;
    QPointer<EditorSession> session_;   // the window destroys its sessions before Qt deletes child dialogs
    compositor::FilterKind kind_;
    compositor::FilterSettings settings_;
    uint32_t seed_;
    QCheckBox* preview_;
    int margin_ = 0;
    std::shared_ptr<const compositor::Image> source_;
    compositor::LayerTransform transform_;
    std::shared_ptr<const compositor::Image> previewSource_;
    double previewScale_ = 1;
    std::shared_ptr<compositor::GrayImage> coverage_;
    std::shared_ptr<compositor::GrayImage> previewCoverage_;
    std::vector<std::function<void()>> syncers_;
    bool finished_ = false;
};

/// Filter > Remove Background: the subject mask from the model, refined live, applied as a layer mask.
class BackgroundDialog : public QDialog {
    Q_OBJECT
public:
    /// `quickModelPath`, when set, is a fast coarse model run first for an instant preview.
    BackgroundDialog(EditorSession* session, QString modelPath, QString quickModelPath = {}, QWidget* parent = nullptr);
    ~BackgroundDialog() override;
protected:
    void done(int result) override;
private:
    void refreshPreview();
    std::shared_ptr<compositor::GrayImage> refined(int limit) const;
    QPointer<EditorSession> session_;   // the window destroys its sessions before Qt deletes child dialogs
    QString modelPath_;
    QCheckBox* preview_;
    QWidget* advanced_;
    bool advancedMode_ = false;
    compositor::MatteSettings settings_;
    std::shared_ptr<const compositor::Image> source_;
    compositor::LayerTransform transform_;
    std::shared_ptr<compositor::GrayImage> raw_;        // the mask the sliders refine: coarse or detailed
    std::shared_ptr<compositor::GrayImage> coarse_, detailed_;
    bool detail_ = false;
    void startDetail();
    QString error_;
    bool finished_ = false;
    bool computing_ = false;
    std::thread worker_;   // runs the model; joined before the dialog goes, so a quit mid-run waits for it
};

} // namespace app
