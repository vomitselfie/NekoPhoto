// Edit > Content-Aware Fill: the document with the selection outlined and the searched area tinted green; Auto
// searches around the selection and Wide Area a larger window around it. Output to the current or a new layer.
// Photoshop's hand-painted sampling area is deliberately not offered (docs/legal-boundaries.md).
#pragma once
#include "EditorSession.h"
#include <QDialog>
#include <QImage>
#include <QPointer>

class QButtonGroup;
class QComboBox;

namespace app {

class SamplingCanvas;

class ContentFillDialog : public QDialog {
    Q_OBJECT
public:
    ContentFillDialog(EditorSession* session, QWidget* parent);
    ~ContentFillDialog() override;
    /// The settings as chosen (the custom area at the document's size).
    ContentFillRequest request() const;

private:
    void previewFill();
    void apply();
    QPointer<EditorSession> session_;
    SamplingCanvas* canvas_ = nullptr;
    QButtonGroup* sampling_ = nullptr;
    QComboBox* output_ = nullptr;
    QImage autoArea_, wideArea_;
    bool previewShown_ = false;
};

/// The dialog's picture: the document scaled to fit, the selection in red and the sampling area in green.
class SamplingCanvas : public QWidget {
    Q_OBJECT
public:
    SamplingCanvas(const QImage& picture, const QImage& selection, QWidget* parent = nullptr);
    void setAutoArea(const QImage& area) { autoArea_ = area; update(); }
    QSize sizeHint() const override { return picture_.size(); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage picture_, selection_, autoArea_;
};

} // namespace app
