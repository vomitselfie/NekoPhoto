// The context bar under the menu: the options of the current tool.
#pragma once
#include "EditorSession.h"
#include <QDoubleSpinBox>
#include <QStackedWidget>
#include <QToolBar>
#include <functional>
#include <vector>

class QCheckBox;
class QHBoxLayout;
class QResizeEvent;

namespace app {

class CanvasWidget;

class ToolOptionsBar : public QToolBar {
    Q_OBJECT
public:
    ToolOptionsBar(EditorSession* session, CanvasWidget* canvas, QWidget* parent = nullptr);

    /// The bar never dictates the window's width: narrow windows clip its right end (see applyCompact).
    QSize minimumSizeHint() const override { return QSize(200, QToolBar::minimumSizeHint().height()); }

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    /// Narrow windows lose the percent fields and get shorter checkbox labels (level 1), then the transform fields (level 2).
    void applyCompact();
    int compact_ = 0;
    QCheckBox* controlsCheck_ = nullptr;
    QCheckBox* ratioCheck_ = nullptr;
    void syncTool();
    void syncTransformFields();
    void applyTransformField();
    QWidget* buildMoveOptions();
    QWidget* buildBrushOptions();
    QWidget* buildMarqueeOptions();
    QWidget* buildLassoOptions();
    QWidget* buildWandOptions();
    QWidget* buildCropOptions();
    QWidget* buildZoomOptions();
    QWidget* buildEyedropperOptions();
    QWidget* buildHealingOptions();
    QWidget* buildCloneOptions();
    QWidget* buildSmudgeOptions();
    QWidget* buildGradientOptions();
    QWidget* buildShapeOptions();
    std::vector<std::function<void()>> syncers_;
    void addBrushTipFields(QHBoxLayout* layout);

    EditorSession* session_;
    CanvasWidget* canvas_;
    QStackedWidget* stack_;
    QDoubleSpinBox *xField_, *yField_, *wField_, *hField_, *wPercent_, *hPercent_, *angleField_;
    bool syncingFields_ = false;
    QWidget* transformFields_;
};

} // namespace app
