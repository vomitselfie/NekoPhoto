// The frame shared by the modeless dialogs that preview on the canvas and then change one layer's pixels:
// Image > Adjustments, the Filter menu, G'MIC and Remove Background.
#pragma once
#include "nekophoto/colormgmt.h"   // TransferCurve
#include "nekophoto/document.h"
#include <QDialog>
#include <QJsonObject>
#include <QPointer>

class QCheckBox;
class QVBoxLayout;

namespace app {

class EditorSession;

/// Pins itself to the layer that was active when it opened: the source pixels, the selection on their grid,
/// every preview and the result go to that layer whatever becomes active meanwhile. Closes itself when its
/// tab's session goes, and clears an uncommitted preview when it closes.
class PixelDialog : public QDialog {
    Q_OBJECT
public:
    ~PixelDialog() override;

protected:
    PixelDialog(EditorSession* session, QWidget* parent);

    /// Takes the pinned layer's pixels, grown by `margin` for blurs, with the selection on their grid and
    /// copies reduced to `previewLimit` on the longest side for previewing (0 previews at full size). In a 16- or
    /// 32-bit document the accessors of that depth hold them (source16(), sourceF() and so on) and the others are null.
    void capture(int margin, int previewLimit);
    /// The same for a filter that works on any depth and layout: the pinned layer's own buffer in sourceNative() and
    /// the selection on its grid in coverageNative(), whatever the mode (the RGB accessors stay null), previewed at
    /// full size.
    void captureAny(int margin);
    /// Adds the Preview check box and the OK / Cancel buttons at the bottom of `layout`; returns the check box.
    QCheckBox* addPreviewAndButtons(QVBoxLayout* layout);
    bool previewing() const;

    /// Shows `image`, computed from previewSource(), through the selection. Ignored once the dialog is done.
    void showPreview(std::shared_ptr<nekophoto::Image> image, std::optional<nekophoto::LayerTransform> placement = std::nullopt);
    void showPreview(std::shared_ptr<nekophoto::Image16> image, std::optional<nekophoto::LayerTransform> placement = std::nullopt);
    void showPreview(std::shared_ptr<nekophoto::ImageF> image, std::optional<nekophoto::LayerTransform> placement = std::nullopt);
    /// In a CMYK or Lab document: `image` at the document's layout, computed from sourceNative().
    void showPreviewNative(nekophoto::AnyImage image, std::optional<nekophoto::LayerTransform> placement = std::nullopt);
    void clearPreview();
    /// Blends a full-size result computed from source() through the selection.
    void throughSelection(nekophoto::Image& result) const;
    void throughSelection(nekophoto::Image16& result) const;
    void throughSelection(nekophoto::ImageF& result) const;
    nekophoto::AnyImage throughSelectionNative(const nekophoto::AnyImage& result) const;
    /// Replaces the pinned layer's pixels as one undo step and marks the dialog finished.
    void commit(nekophoto::AnyImage image, const nekophoto::LayerTransform& placement, const QString& name);
    /// The command path (CONTRIBUTING.md, "Commands"): OK runs automation method `method` with `params` on the pinned
    /// layer instead of committing the dialog's own result, so the menu, automation and Actions share one edit. True
    /// when it ran (an error is shown by the window), false when the command path is not available here (another
    /// tab is on screen, the layer went): the caller commits as before.
    bool commitAsCommand(const QString& method, const QJsonObject& params);
    /// Closes with `result` without committing anything further (after an asynchronous commit, say).
    void finish(int result);

    /// OK: commit and return true, or return false to stay open (still computing, or committing later).
    virtual bool apply() = 0;
    void done(int result) override;

    EditorSession* session() const;
    const std::shared_ptr<const nekophoto::Image>& source() const { return source_; }
    const std::shared_ptr<const nekophoto::Image>& previewSource() const { return previewSource_; }
    const std::shared_ptr<const nekophoto::Image16>& source16() const { return source16_; }
    const std::shared_ptr<const nekophoto::Image16>& previewSource16() const { return previewSource16_; }
    /// In a 32-bit document, and the curve its colour is encoded with (for the adjustments and Add Noise).
    const std::shared_ptr<const nekophoto::ImageF>& sourceF() const { return sourceF_; }
    const std::shared_ptr<const nekophoto::ImageF>& previewSourceF() const { return previewSourceF_; }
    const nekophoto::TransferCurve& curve() const { return curve_; }
    /// In a CMYK or Lab document the layer's own samples (modeedit.h), previewed at full size; the RGB accessors are
    /// null there, so no RGB kernel ever meets them.
    const nekophoto::AnyImage& sourceNative() const { return sourceNative_; }
    const nekophoto::AnyGray& coverageNative() const { return coverageNative_; }
    nekophoto::ColorMode colorMode() const { return mode_; }
    const nekophoto::ColorProfile& documentProfile() const { return profile_; }
    /// Whether the pinned layer's pixels were captured at all, at any depth.
    bool hasSource() const { return source_ || source16_ || sourceF_ || sourceNative_; }
    bool hasPreviewSource() const { return previewSource_ || previewSource16_ || previewSourceF_ || sourceNative_; }
    const nekophoto::LayerTransform& placement() const { return transform_; }
    /// The selection on source()'s grid, or null when everything is selected.
    const nekophoto::GrayImage* coverage() const { return coverage_.get(); }
    const std::optional<nekophoto::Uuid>& layerId() const { return layerId_; }
    double previewScale() const { return previewScale_; }
    bool finished() const { return finished_; }

private:
    QPointer<EditorSession> session_;   // the window destroys its sessions before Qt deletes child dialogs
    std::optional<nekophoto::Uuid> layerId_;
    std::shared_ptr<const nekophoto::Image> source_, previewSource_;
    nekophoto::LayerTransform transform_;
    double previewScale_ = 1;
    std::shared_ptr<nekophoto::GrayImage> coverage_, previewCoverage_;
    std::shared_ptr<const nekophoto::Image16> source16_, previewSource16_;
    std::shared_ptr<nekophoto::Gray16> coverage16_, previewCoverage16_;
    std::shared_ptr<const nekophoto::ImageF> sourceF_, previewSourceF_;
    std::shared_ptr<nekophoto::GrayF> coverageF_, previewCoverageF_;
    nekophoto::TransferCurve curve_;
    nekophoto::AnyImage sourceNative_;
    nekophoto::AnyGray coverageNative_;
    nekophoto::ColorMode mode_ = nekophoto::ColorMode::RGB;
    nekophoto::ColorProfile profile_;
    QCheckBox* preview_ = nullptr;
    bool finished_ = false;
};

} // namespace app
