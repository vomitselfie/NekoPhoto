// The Camera Raw dialog a camera RAW file opens in, as in Photoshop: a quick half-size decode previews at once, the
// Camera Raw panels grade it, and Open develops the whole file (off the UI thread, cancellable) into a new document,
// Open Object into a new document whose layer is a smart object holding the RAW file and the settings, Cancel opens
// nothing. Edit Contents on such a smart object reopens it here with its settings (OK develops it again).
#pragma once
#include "compositor/cameraraw.h"
#include "compositor/imaget.h"
#include <QDialog>
#include <QImage>
#include "compositor/raw.h"
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace app {

class CameraRawPanels;

class RawDevelopDialog : public QDialog {
    Q_OBJECT
public:
    /// Open: Open, Open Object and Cancel, with the workflow's depth. Redevelop: OK and Cancel (Edit Contents).
    enum class Purpose { Open, Redevelop };
    enum class Choice { Cancelled, Open, OpenObject };
    RawDevelopDialog(std::shared_ptr<const std::vector<uint8_t>> bytes, const QString& fileName, const compositor::CameraRawSettings& settings,
                     Purpose purpose, QWidget* parent = nullptr);
    ~RawDevelopDialog() override;

    Choice choice() const { return choice_; }
    const compositor::CameraRawSettings& settings() const;
    /// The whole file developed with settings(), at 16 bits; set once the dialog is accepted.
    const std::shared_ptr<compositor::Image16>& developed() const { return developed_; }
    /// Camera Raw's workflow depth for Open (8 or 16; 16 by default, remembered as Photoshop does).
    int bitsPerChannel() const;

    /// The workflow depth the dialog last used (Preferences-free, as in Photoshop: set in the dialog).
    static int workflowBits();

protected:
    void reject() override;

private:
    /// The quick decode at the current white balance's multipliers, off the UI thread; one at a time.
    void startPreviewDecode();
    void previewDecoded(std::shared_ptr<compositor::Image16> image, const std::string& error, const std::array<double, 3>& multipliers,
                        int generation);
    void stopPreview();
    /// The multipliers the settings develop with.
    std::array<double, 3> targetMultipliers() const;
    void refreshPreview();
    void showPreviewImage();
    void develop(Choice choice);
    void developed(Choice choice);
    void stopWorker();
    void resizeEvent(QResizeEvent* event) override;

    std::shared_ptr<const std::vector<uint8_t>> bytes_;
    Purpose purpose_;
    Choice choice_ = Choice::Cancelled;
    CameraRawPanels* panels_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* status_ = nullptr;
    QComboBox* depth_ = nullptr;
    QPushButton* open_ = nullptr;
    QPushButton* openObject_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QTimer* debounce_ = nullptr;
    int fullWidth_ = 0, fullHeight_ = 0;
    compositor::RawWhiteBalance balance_;
    std::shared_ptr<compositor::Image16> base_;      // the quick decode, reduced for previewing
    std::array<double, 3> baseMultipliers_{1, 1, 1};   // the multipliers base_ was decoded with
    std::thread previewWorker_;
    std::atomic<bool> previewCancel_{false};
    int previewGeneration_ = 0;
    bool previewBusy_ = false;
    QTimer* settle_ = nullptr;
    QImage shown_;
    std::shared_ptr<compositor::Image16> developed_;
    std::shared_ptr<compositor::Image16> workerResult_;
    std::string workerError_;
    std::atomic<bool> cancelFlag_{false};
    std::thread worker_;
    bool developing_ = false;
};

} // namespace app
