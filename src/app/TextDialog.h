// Editing a text layer letter by letter, as Photoshop's Type tool does: a rich editor holding the text in its
// style runs, a Character section showing the style at the cursor or over the selection (blank where it is mixed)
// and changing it for the selected letters (all of the text when nothing is selected), and the paragraph's
// alignment and line spacing. Changes are applied to the layer as they happen; OK keeps the edit as one undo
// step, Cancel puts the layer back.
#pragma once
#include "EditorSession.h"
#include "FontPicker.h"
#include <QDialog>
#include <QPointer>
#include <QTimer>

class QTextEdit;
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
    /// The Character section shown for the selection (or the cursor).
    void showCharacter();
    /// `patch` given to the selected letters, or all of them when none are.
    void restyle(const compositor::TextRunPatch& patch);

    QPointer<EditorSession> session_;
    compositor::Uuid layerId_;
    compositor::LayerText original_;
    compositor::TextRun fallback_;     // the style of text that holds none (the first run as opened)
    double displayScale_ = 1;
    bool syncing_ = false;
    QTextEdit* editor_;
    FontPicker* family_;
    QDoubleSpinBox* size_;
    QComboBox* weight_;
    QToolButton* bold_;
    QToolButton* italic_;
    QPushButton* colour_;
    QDoubleSpinBox* tracking_;
    QDoubleSpinBox* baseline_;
    QDoubleSpinBox* leading_;
    QComboBox* caps_;
    QToolButton* underline_;
    QToolButton* strike_;
    QComboBox* alignment_;
    QDoubleSpinBox* lineSpacing_;
    QTimer debounce_;
    bool finished_ = false;
};

} // namespace app
