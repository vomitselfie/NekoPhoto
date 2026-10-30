// Modeless dialogs for the destructive adjustments (Image > Adjustments) and
// filters (Filter menu), previewing on the canvas and committing on OK.
#pragma once
#include "AdjustmentEditor.h"
#include "PixelDialog.h"
#include "nekophoto/filters.h"
#include "nekophoto/matte.h"
#include <thread>

class QCheckBox;
class QTimer;

namespace app {

/// Image > Adjustments > Levels / Curves / Hue-Saturation / Exposure / Gradient Map / Grain on pixels.
class PixelAdjustmentDialog : public PixelDialog {
    Q_OBJECT
public:
    PixelAdjustmentDialog(EditorSession* session, nekophoto::AdjustmentKind kind, QWidget* parent = nullptr);
protected:
    bool apply() override;
private:
    void refreshPreview();
    std::shared_ptr<nekophoto::Image> run(const nekophoto::Image& source, double scale) const;
    std::shared_ptr<nekophoto::Image16> run(const nekophoto::Image16& source, double scale) const;
    std::shared_ptr<nekophoto::ImageF> run(const nekophoto::ImageF& source, double scale) const;
    AdjustmentEditor* editor_;
};

/// Filter > Gaussian Blur / Motion Blur / Add Noise / Lens Correction, and the grid filters (nekophoto/filters.h,
/// applyGridFilter: Box Blur to Mosaic through the Smart Filter kernels, the PhotoCraft ports), which preview at full
/// size on the layer's own buffer, a moment after the last change.
class FilterDialog : public PixelDialog {
    Q_OBJECT
public:
    /// `smart`: on a smart object, OK adds the filter as a Smart Filter instead of changing pixels.
    FilterDialog(EditorSession* session, nekophoto::FilterKind kind, QWidget* parent = nullptr, bool smart = false);
    /// Add Noise's seed (random per dialog); for checks that compare runs.
    void setSeed(uint32_t seed) { seed_ = seed % 1000000000u; }
protected:
    bool apply() override;
private:
    void prepareSource();
    void refreshPreview();
    bool identity() const;
    std::shared_ptr<nekophoto::Image> run(const nekophoto::Image& source, double scale) const;
    std::shared_ptr<nekophoto::Image16> run(const nekophoto::Image16& source, double scale) const;
    std::shared_ptr<nekophoto::ImageF> run(const nekophoto::ImageF& source, double scale) const;
    nekophoto::FilterKind kind_;
    bool smart_ = false;
    nekophoto::FilterSettings settings_;
    uint32_t seed_;
    int margin_ = -1;
    std::vector<std::function<void()>> syncers_;
    QTimer* gridTimer_ = nullptr;   // the grid filters' preview, coalesced
    void refreshGridPreview();
    nekophoto::AnyImage runGrid() const;
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
    std::shared_ptr<nekophoto::GrayImage> refined(int limit) const;
    /// The refined matte as a float plane, for a 16-bit layer's 16-bit mask.
    nekophoto::AlphaPlane refinedPlane(int limit) const;
    QString modelPath_;
    /// What the model and the refinement see: the layer's pixels, reduced to 8 bits on a 16-bit layer.
    std::shared_ptr<const nekophoto::Image> guide_;
    QWidget* advanced_;
    bool advancedMode_ = false;
    nekophoto::MatteSettings settings_;
    std::shared_ptr<nekophoto::GrayImage> raw_;        // the mask the sliders refine: coarse or detailed
    std::shared_ptr<nekophoto::GrayImage> coarse_, detailed_;
    bool detail_ = false;
    bool mirror_ = false;   // Preferences' mirrored pass, as it was when the dialog opened
    QString error_;
    bool computing_ = false;
    std::thread worker_;   // runs the model; joined before the dialog goes, so a quit mid-run waits for it
};

} // namespace app
