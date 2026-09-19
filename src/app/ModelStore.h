// Where the Remove Background models live, which one is chosen, and fetching
// them. Models are rembg's ONNX releases (Apache-2.0), kept under the app's
// data directory rather than shipped in the repository. The feature is off
// until turned on in Preferences, so nothing is downloaded unasked.
#pragma once
#include <QObject>
#include <QString>
#include <functional>
#include <vector>

class QWidget;

namespace app {

struct ModelInfo {
    QString id;        // settings key
    QString name;      // file name under the models directory
    QString url;
    QString sha256;
    qint64 bytes;
    QString label;
    QString about;
};

class ModelStore : public QObject {
    Q_OBJECT
public:
    /// The models on offer, best first: IS-Net general use, then the small U2Net.
    static const std::vector<ModelInfo>& models();
    static const ModelInfo* modelById(const QString& id);

    /// Preferences: whether the feature is on, and which model it uses.
    static bool enabled();
    static void setEnabled(bool on);
    static const ModelInfo& selected();
    static void setSelected(const QString& id);
    /// Ready to run: enabled, this build has model support, and the chosen model is downloaded.
    static bool ready();
    static bool supported();

    /// The models directory: $COMPOSITOR_MODEL_DIR, else the app data location.
    static QString directory();
    static QString pathFor(const ModelInfo& model);
    static bool isPresent(const ModelInfo& model);
    static bool remove(const ModelInfo& model);

    /// Downloads `model`, verifying its hash. `progress(received, total)` is called as bytes arrive; `done` gets the
    /// path, or an empty path with an error (empty when cancelled). The returned handle cancels the download.
    struct Download { std::function<void()> cancel; };
    static Download download(const ModelInfo& model, QObject* context, std::function<void(qint64, qint64)> progress, std::function<void(QString path, QString error)> done);
};

} // namespace app
