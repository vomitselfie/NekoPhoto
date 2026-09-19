// The Adjustments dock: edits the active adjustment layer's settings live.
#pragma once
#include "AdjustmentEditor.h"
#include "EditorSession.h"
#include <QLabel>
#include <QWidget>

namespace app {

class AdjustmentsPanel : public QWidget {
    Q_OBJECT
public:
    explicit AdjustmentsPanel(EditorSession* session, QWidget* parent = nullptr);

private:
    void sync();
    EditorSession* session_;
    AdjustmentEditor* editor_;
    QLabel* title_;
    std::optional<compositor::Uuid> layerId_;
    bool editing_ = false;
};

} // namespace app
