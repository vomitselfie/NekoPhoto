// Where the Remove Background model lives, and fetching it the first time.
// Models are rembg's ONNX releases (Apache-2.0), kept under the app's data
// directory rather than shipped in the repository: ~180 MB for IS-Net.
#pragma once
#include <QObject>
#include <QString>
#include <functional>

namespace app {

struct ModelInfo {
    QString name;      // file name under the models directory
    QString url;
    QString sha256;
    qint64 bytes;
    QString label;
};

class ModelStore : public QObject {
    Q_OBJECT
public:
    /// IS-Net general use: the best of the permissively licensed models, 1024 px input.
    static const ModelInfo& primary();
    /// u2netp: 4.6 MB, for a quick first result or an offline machine.
    static const ModelInfo& small();
    /// The models directory: $COMPOSITOR_MODEL_DIR, else the app data location.
    static QString directory();
    static QString pathFor(const ModelInfo& model);
    static bool isPresent(const ModelInfo& model);

    /// Downloads `model` with a progress dialog (verifying its hash), then calls `done` with the path or an empty
    /// string on failure or cancel. Asks first, with a note on the size and license.
    static void ensure(const ModelInfo& model, QWidget* parent, std::function<void(QString path, QString error)> done);
};

} // namespace app
