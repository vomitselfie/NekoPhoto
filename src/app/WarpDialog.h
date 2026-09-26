// Edit ▸ Warp: Photoshop's preset warps (Arc, Flag, Bulge, ...) with bend and distortion, for the active layer.
#pragma once
#include "EditorSession.h"
#include <QDialog>
#include <QPointer>

class QComboBox;
class QSpinBox;

namespace app {

class WarpDialog : public QDialog {
    Q_OBJECT
public:
    WarpDialog(EditorSession* session, QWidget* parent = nullptr);
    /// The preset names in Photoshop's order, with the style ids it stores.
    static const std::vector<std::pair<const char*, const char*>>& styles();

protected:
    void accept() override;

private:
    QPointer<EditorSession> session_;
    QComboBox* style_;
    QComboBox* orientation_;
    QSpinBox* bend_;
    QSpinBox* horizontal_;
    QSpinBox* vertical_;
};

} // namespace app
