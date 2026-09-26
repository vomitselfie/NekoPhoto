// A Smart Filter entry's dialogs, opened from the Layers panel: its settings (double-click on the entry) and its
// Blending Options (opacity and blend mode). The canvas previews the change; OK applies it as one undo step.
#pragma once
#include "EditorSession.h"
#include <QDialog>
#include <QPointer>
#include <QTimer>

namespace app {

class SmartFilterDialog : public QDialog {
    Q_OBJECT
public:
    enum class Page { Settings, Blending };
    /// Entry `index` (running order) of smart object `layerId`'s stack.
    SmartFilterDialog(EditorSession* session, compositor::Uuid layerId, int index, Page page, QWidget* parent = nullptr);
    ~SmartFilterDialog() override;
    /// Whether the entry is one this dialog can edit (a filter drawn here, on a stack that can change).
    static bool canEdit(const EditorSession* session, const compositor::Uuid& layerId, int index);

protected:
    void done(int result) override;

private:
    void schedulePreview();
    void preview();
    QPointer<EditorSession> session_;
    compositor::Uuid layerId_;
    int index_;
    compositor::SmartFilterEntry entry_;
    QTimer previewTimer_;
    bool previewing_ = false;
};

} // namespace app
