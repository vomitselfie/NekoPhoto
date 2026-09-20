// Filter > G'MIC: the catalogue on the left, the chosen filter's controls on the right, a live preview
// on the canvas, and the exact command line for anyone who wants to edit it.
#pragma once
#include "EditorSession.h"
#include "Gmic.h"
#include <QDialog>
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

class GmicDialog : public QDialog {
    Q_OBJECT
public:
    GmicDialog(EditorSession* session, QWidget* parent = nullptr);
    ~GmicDialog() override;

protected:
    void done(int result) override;

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
    void applyAndClose();

    EditorSession* session_;
    GmicCatalogue catalogue_;
    std::vector<GmicFilter> presets_;
    GmicFilter current_;
    bool customCommand_ = false;

    QLineEdit* search_;
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

    std::shared_ptr<const compositor::Image> source_;
    compositor::LayerTransform transform_;
    std::optional<compositor::Uuid> layerId_;   // the layer this dialog opened on: its preview and result go there, whatever becomes active meanwhile
    std::shared_ptr<const compositor::Image> previewSource_;
    double previewScale_ = 1;
    std::shared_ptr<compositor::GrayImage> coverage_, previewCoverage_;
    bool finished_ = false;
    bool applying_ = false;
};

} // namespace app
