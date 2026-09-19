// Editing a text layer: the text itself, the font, its size and style, colour and alignment, applied to
// the layer as they change; OK keeps the edit as one undo step, Cancel puts the layer back.
#pragma once
#include "EditorSession.h"
#include "FontPicker.h"
#include <QDialog>
#include <QPointer>
#include <QTimer>

class QPlainTextEdit;
class QDoubleSpinBox;
class QToolButton;
class QComboBox;
class QPushButton;

namespace app {

class TextDialog : public QDialog {
    Q_OBJECT
public:
    TextDialog(EditorSession* session, compositor::Uuid layerId, QWidget* parent = nullptr);
    ~TextDialog() override;

protected:
    void done(int result) override;

private:
    void apply();
    void pickColour();

    QPointer<EditorSession> session_;
    compositor::Uuid layerId_;
    compositor::LayerText text_;
    QPlainTextEdit* editor_;
    FontPicker* family_;
    QDoubleSpinBox* size_;
    QToolButton* bold_;
    QToolButton* italic_;
    QComboBox* alignment_;
    QDoubleSpinBox* lineSpacing_;
    QDoubleSpinBox* letterSpacing_;
    QPushButton* colour_;
    QTimer debounce_;
    bool finished_ = false;
};

} // namespace app
