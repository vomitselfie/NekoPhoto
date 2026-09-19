// G'MIC (gmic.eu), the open-source filter framework: the `gmic` executable runs a filter on a layer's
// pixels through a PNG round trip, and the filter catalogue comes from G'MIC's own definition file
// (the `#@gui` lines the G'MIC-Qt plugin reads), so every filter it lists is available here.
#pragma once
#include "compositor/image.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

class QProcess;
class QTemporaryDir;

namespace app {

/// One parameter of a catalogue filter, as declared in its `#@gui :Label=type(...)` line.
struct GmicParam {
    enum Kind { Float, Int, Bool, Choice, Color, Text, Note, Separator, Point, Value, Unsupported };
    Kind kind = Unsupported;
    QString label;
    double value = 0, min = 0, max = 1;   // Float, Int
    int decimals = 2;
    QStringList choices;                  // Choice (value = index)
    QString text;                         // Text, Note; Value / Point keep their literal argument here
    double r = 0, g = 0, b = 0, a = 255;  // Color
    bool hasAlpha = false;
    /// The argument this parameter contributes to the command line (empty for notes and separators).
    QString argument() const;
    bool contributes() const { return kind != Note && kind != Separator; }
};

struct GmicFilter {
    QString name, folder, command, previewCommand;
    std::vector<GmicParam> params;
    /// `command arg,arg,...` for the full run (or the preview variant).
    QString commandLine(bool preview) const;
};

/// The filter definitions: G'MIC's update file, parsed for `#@gui` entries.
class GmicCatalogue {
public:
    bool load(const QString& path, QString* error = nullptr);
    const std::vector<GmicFilter>& filters() const { return filters_; }
    const QString& source() const { return source_; }
    /// The definition file to use: ours (downloaded through Update Filters), else G'MIC-Qt's copy if present.
    static QString preferredFile();
    /// Where Update Filters saves the definitions for the installed G'MIC version.
    static QString ownFile();
    static QString updateUrl();

private:
    std::vector<GmicFilter> filters_;
    QString source_;
};

/// Runs G'MIC commands on images through the `gmic` executable.
class GmicRunner : public QObject {
    Q_OBJECT
public:
    explicit GmicRunner(QObject* parent = nullptr);
    ~GmicRunner() override;

    static QString executable();
    static QString version();
    static QStringList tokenize(const QString& command);
    /// Runs `command` on premultiplied RGBA `source`; the result has the same size or `error` says why not.
    static std::shared_ptr<compositor::Image> runSync(const compositor::Image& source, const QString& command, QString* error, int timeoutMs = 300000);

    /// Starts an asynchronous run; `finished` reports the result (null on failure) and the error.
    void start(std::shared_ptr<const compositor::Image> source, const QString& command);
    void cancel();
    bool running() const;

signals:
    void finished(std::shared_ptr<compositor::Image> result, QString error);

private:
    QProcess* process_ = nullptr;
    std::unique_ptr<QTemporaryDir> dir_;
    int expectedWidth_ = 0, expectedHeight_ = 0;
};

} // namespace app
