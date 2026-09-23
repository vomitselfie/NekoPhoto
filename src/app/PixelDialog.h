// The frame shared by the modeless dialogs that preview on the canvas and then change one layer's pixels:
// Image > Adjustments, the Filter menu, G'MIC and Remove Background.
#pragma once
#include "EditorSession.h"
#include <QDialog>
#include <QPointer>

class QCheckBox;
class QVBoxLayout;

namespace app {

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
    /// copies reduced to `previewLimit` on the longest side for previewing (0 previews at full size).
    void capture(int margin, int previewLimit);
    /// Adds the Preview check box and the OK / Cancel buttons at the bottom of `layout`; returns the check box.
    QCheckBox* addPreviewAndButtons(QVBoxLayout* layout);
    bool previewing() const;

    /// Shows `image`, computed from previewSource(), through the selection. Ignored once the dialog is done.
    void showPreview(std::shared_ptr<compositor::Image> image, std::optional<compositor::LayerTransform> placement = std::nullopt);
    void clearPreview();
    /// Blends a full-size result computed from source() through the selection.
    void throughSelection(compositor::Image& result) const;
    /// Replaces the pinned layer's pixels as one undo step and marks the dialog finished.
    void commit(std::shared_ptr<const compositor::Image> image, const compositor::LayerTransform& placement, const QString& name);
    /// Closes with `result` without committing anything further (after an asynchronous commit, say).
    void finish(int result);

    /// OK: commit and return true, or return false to stay open (still computing, or committing later).
    virtual bool apply() = 0;
    void done(int result) override;

    EditorSession* session() const { return session_; }
    const std::shared_ptr<const compositor::Image>& source() const { return source_; }
    const std::shared_ptr<const compositor::Image>& previewSource() const { return previewSource_; }
    const compositor::LayerTransform& placement() const { return transform_; }
    /// The selection on source()'s grid, or null when everything is selected.
    const compositor::GrayImage* coverage() const { return coverage_.get(); }
    const std::optional<compositor::Uuid>& layerId() const { return layerId_; }
    double previewScale() const { return previewScale_; }
    bool finished() const { return finished_; }

private:
    QPointer<EditorSession> session_;   // the window destroys its sessions before Qt deletes child dialogs
    std::optional<compositor::Uuid> layerId_;
    std::shared_ptr<const compositor::Image> source_, previewSource_;
    compositor::LayerTransform transform_;
    double previewScale_ = 1;
    std::shared_ptr<compositor::GrayImage> coverage_, previewCoverage_;
    QCheckBox* preview_ = nullptr;
    bool finished_ = false;
};

} // namespace app
