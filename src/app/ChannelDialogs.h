// Photoshop's Select ▸ Save Selection and Load Selection, and the Channels panel's Channel Options. Each applies
// itself to the session when accepted (one undo step) and deletes itself when closed; see docs/channels.md.
#pragma once
#include "EditorSession.h"
#include <QDialog>
#include <QPointer>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace app {

/// Save Selection: into a new channel (with a name) or into an alpha channel: replace, add, subtract or intersect.
class SaveSelectionDialog : public QDialog {
    Q_OBJECT
public:
    SaveSelectionDialog(EditorSession* session, QWidget* parent = nullptr);

private:
    void refresh();
    void apply();
    QPointer<EditorSession> session_;
    QComboBox* channel_;
    QLineEdit* name_;
    QButtonGroup* operation_;
};

/// Load Selection: from an alpha channel, a layer's transparency or a layer's mask, optionally inverted, as a new
/// selection or added to, subtracted from or intersected with the current one.
class LoadSelectionDialog : public QDialog {
    Q_OBJECT
public:
    LoadSelectionDialog(EditorSession* session, QWidget* parent = nullptr);

private:
    void apply();
    QPointer<EditorSession> session_;
    QComboBox* source_;
    QCheckBox* invert_;
    QButtonGroup* operation_;
};

/// Channel Options: name, what the colour indicates (masked or selected areas; a spot channel's is its ink), the
/// overlay colour and its opacity (a spot channel's solidity).
class ChannelOptionsDialog : public QDialog {
    Q_OBJECT
public:
    ChannelOptionsDialog(EditorSession* session, const compositor::Uuid& channel, QWidget* parent = nullptr);

private:
    void setSwatch(const QColor& color);
    void apply();
    QPointer<EditorSession> session_;
    compositor::Uuid id_;
    QLineEdit* name_;
    QRadioButton* masked_;
    QRadioButton* selected_;
    QPushButton* swatch_;
    QDoubleSpinBox* opacity_;
    QColor color_;
};

} // namespace app
