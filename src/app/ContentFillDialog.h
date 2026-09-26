// Edit > Content-Aware Fill: Photoshop's workspace in a dialog. The document with the selection outlined and the
// sampling area tinted green; Auto samples around the selection, All the whole layer, and Custom the area painted
// with the sampling brush (left button adds, right button or Alt removes). Output to the current or a new layer.
#pragma once
#include "EditorSession.h"
#include <QDialog>
#include <QImage>
#include <QPointer>

class QButtonGroup;
class QComboBox;
class QSpinBox;

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
    QSpinBox* brushSize_ = nullptr;
    bool previewShown_ = false;
};

/// The dialog's picture: the document scaled to fit, the selection in red and the sampling area in green.
class SamplingCanvas : public QWidget {
    Q_OBJECT
public:
    SamplingCanvas(const QImage& picture, const QImage& selection, QWidget* parent = nullptr);
    void setCustom(bool custom) { custom_ = custom; update(); }
    void setAutoArea(const QImage& area) { autoArea_ = area; update(); }
    void setAll(bool all) { all_ = all; update(); }
    void setBrush(int diameter) { brush_ = diameter; }
    /// The painted area at picture scale (white = sample here).
    const QImage& area() const { return area_; }
    QSize sizeHint() const override { return picture_.size(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void dab(QPointF at, bool add);
    QImage picture_, selection_, area_, autoArea_;
    bool custom_ = false, all_ = false;
    int brush_ = 24;
};

} // namespace app
