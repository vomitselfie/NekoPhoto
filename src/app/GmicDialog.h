// Filter > G'MIC: the catalogue on the left, the chosen filter's controls on the right, a live preview
// on the canvas, and the exact command line for anyone who wants to edit it.
#pragma once
#include "Gmic.h"
#include "PixelDialog.h"
#include <QTimer>
#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;
class QNetworkAccessManager;

namespace app {

class GmicDialog : public PixelDialog {
    Q_OBJECT
public:
    GmicDialog(EditorSession* session, QWidget* parent = nullptr);
    ~GmicDialog() override;

protected:
    bool apply() override;

private:
    void loadCatalogue();
    void fillTree(const QString& search);
    void selectFilter(const GmicFilter* filter);
    void buildControls();
    void updateCommand();
    void schedulePreview();
    void runPreview();
    void previewFinished(std::shared_ptr<compositor::Image> result, QString error);
    void updateFilters();

    GmicCatalogue catalogue_;
    std::vector<GmicFilter> presets_;
    GmicFilter current_;
    bool customCommand_ = false;

    QLineEdit* search_;
    QCheckBox* showAll_ = nullptr;
    QTreeWidget* tree_;
    QWidget* controls_;
    QVBoxLayout* controlsLayout_;
    QLineEdit* command_;
    QCheckBox* preview_;
    QLabel* status_;
    QLabel* catalogueInfo_;
    QPushButton* update_;
    QPushButton* ok_;
    QTimer debounce_;
    GmicRunner preview_runner_;
    QNetworkAccessManager* network_ = nullptr;
    bool applying_ = false;
};

} // namespace app
