// File > Export > Export As… and Layer > Export As… (Photoshop's): one dialog for the flat formats, with a live
// preview of the encoded file, its estimated size, the image size and resampling, transparency or a matte, and
// colour conversion. Each format's settings are remembered (ExportAs.h). The dialog only chooses: the caller asks
// where to write and commits through document.export with commandParams().
#pragma once
#include "ExportAs.h"
#include <QDialog>
#include <QMap>
#include <map>
#include <optional>
#include <tuple>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace app {

class EditorSession;

class ExportAsDialog : public QDialog {
    Q_OBJECT
public:
    /// `layer`: the active layer alone, cropped to its visible pixels. `format` starts on that format (a key from
    /// exportas::formats()); empty starts on the one used last.
    ExportAsDialog(EditorSession* session, bool layer, const QString& format = {}, QWidget* parent = nullptr);
    /// False when there is nothing to export (no document, or a layer without visible pixels); why() says why.
    bool ready() const { return ready_; }
    QString why() const { return why_; }

    QString format() const;
    exportas::Settings settings() const;
    /// The size the file will have.
    int outputWidth() const;
    int outputHeight() const;
    /// The image's own size (a layer's visible pixels, or the canvas).
    QSize sourceSize() const { return sourceSize_; }
    /// document.export's parameters for the choices made (the caller adds path and overwrite).
    QJsonObject commandParams() const;
    /// Saves the chosen format's settings and the format itself, for the next Export As and Quick Export.
    void rememberSettings();

    /// The last estimate in bytes; exact when the preview encoded the file at its full size.
    qint64 estimatedBytes() const { return estimate_; }
    bool estimateExact() const { return exact_; }
    /// Updates the preview and estimate now instead of after the pause that follows a change.
    void refreshNow();

    void setFormat(const QString& format);
    void setScalePercent(double percent);
    void setOutputWidth(int width);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void loadSettings(const QString& format);
    void storeSettings();
    void syncEnabled();
    void schedule();
    void refresh();
    void syncSize(QObject* from);
    const exportas::Flat& flat();

    EditorSession* session_;
    bool layer_ = false, ready_ = false;
    QString why_;
    std::optional<compositor::Document> solo_;
    const compositor::Document* source_ = nullptr;
    QSize sourceSize_;
    QString format_;
    QMap<QString, exportas::Settings> settings_;
    std::map<std::tuple<bool, bool, bool>, exportas::Flat> flats_;   // by (16 bits, convert, embed)
    QColor matte_ = Qt::white;
    qint64 estimate_ = 0;
    bool exact_ = false, syncing_ = false;

    QLabel* preview_ = nullptr;
    QComboBox* formatBox_ = nullptr;
    QSlider* quality_ = nullptr;
    QSpinBox* qualitySpin_ = nullptr;
    QWidget* qualityRow_ = nullptr;
    QLabel* qualityLabel_ = nullptr;
    QCheckBox* transparency_ = nullptr;
    QPushButton* matteButton_ = nullptr;
    QLabel* matteLabel_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QDoubleSpinBox* scale_ = nullptr;
    QCheckBox* keepRatio_ = nullptr;
    QComboBox* resample_ = nullptr;
    QCheckBox* convert_ = nullptr;
    QCheckBox* embed_ = nullptr;
    QLabel* colorNote_ = nullptr;
    QLabel* formatNote_ = nullptr;
    QLabel* estimateLabel_ = nullptr;
    QTimer* timer_ = nullptr;
};

} // namespace app
