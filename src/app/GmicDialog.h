// Filter > G'MIC: the catalogue on the left, the chosen filter's controls on the right, a live preview
// on the canvas, and the exact command line for anyone who wants to edit it.
#pragma once
#include "Gmic.h"
#include "PixelDialog.h"
#include <QTimer>
#include <memory>
#include <utility>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;
class QNetworkAccessManager;
class QProgressBar;

namespace app {

class GmicStore;

class GmicDialog : public PixelDialog {
    Q_OBJECT
public:
    GmicDialog(EditorSession* session, QWidget* parent = nullptr);
    ~GmicDialog() override;
    /// Shows the filter named `name` (an essential or a catalogue filter), as Edit > Search picks it.
    void showFilter(const QString& name);
    /// The filters the dialog lists at its defaults, as {name, folder}: the essentials, then the catalogue's
    /// filters that work here. Read once (the catalogue file is parsed on first use).
    static const std::vector<std::pair<QString, QString>>& listedFilters();

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
    void previewFinished16(std::shared_ptr<compositor::Image16> result, QString error);
    void updateFilters();
    void downloadGmic();
    void gmicInstalled();

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
    // G'MIC missing: the offer to download it (Windows), or how to install it.
    QWidget* installBox_ = nullptr;
    QLabel* installText_ = nullptr;
    QPushButton* installButton_ = nullptr;
    QPushButton* installCancel_ = nullptr;
    QProgressBar* installProgress_ = nullptr;
    GmicStore* store_ = nullptr;
    bool applying_ = false;
};

} // namespace app
