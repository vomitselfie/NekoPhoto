// Edit > Preferences: the AI background removal switch, model choice and
// download, plus where the models live.
#pragma once
#include "ModelStore.h"
#include <QDialog>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;

namespace app {

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget* parent = nullptr);
    ~PreferencesDialog() override;

signals:
    /// The feature's readiness may have changed (enabled, downloaded, removed).
    void backgroundRemovalChanged();

private:
    void syncStatus();
    void startDownload();
    void removeModel();
    const ModelInfo& chosen() const;

    QCheckBox* enable_;
    QComboBox* model_;
    QLabel* about_;
    QLabel* status_;
    QLabel* location_;
    QProgressBar* progress_;
    QPushButton* download_;
    QPushButton* remove_;
    QPushButton* cancel_;
    std::optional<ModelStore::Download> active_;
};

} // namespace app
