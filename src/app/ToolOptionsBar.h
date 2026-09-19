// The context bar under the menu: the options of the current tool.
#pragma once
#include "EditorSession.h"
#include <QDoubleSpinBox>
#include <QStackedWidget>
#include <QToolBar>

namespace app {

class CanvasWidget;

class ToolOptionsBar : public QToolBar {
    Q_OBJECT
public:
    ToolOptionsBar(EditorSession* session, CanvasWidget* canvas, QWidget* parent = nullptr);

private:
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

    EditorSession* session_;
    CanvasWidget* canvas_;
    QStackedWidget* stack_;
    QDoubleSpinBox *xField_, *yField_, *wField_, *hField_, *wPercent_, *hPercent_, *angleField_;
    bool syncingFields_ = false;
    QWidget* transformFields_;
};

} // namespace app
