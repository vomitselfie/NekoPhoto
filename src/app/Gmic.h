// G'MIC (gmic.eu), the open-source filter framework: a filter runs on a layer's pixels through the
// `gmic` executable and a PNG round trip, or, when the build found libgmic and COMPOSITOR_GMIC_INPROCESS
// is set, in-process through one interpreter kept warm with the catalogue's commands; the filter
// catalogue comes from G'MIC's own definition file (the `#@gui` lines the G'MIC-Qt plugin reads), so
// every filter it lists is available.
#pragma once
#include "compositor/image.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <thread>
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

    /// G'MIC can run: the library is built in, or the executable is on PATH.
    static bool available();
    /// Filters run in-process through libgmic (no PNG round trip): built in and COMPOSITOR_GMIC_INPROCESS set.
    static bool inProcess();
    static QString executable();
    static QString version();
    static QStringList tokenize(const QString& command);
    /// Whether automation may run `command`. G'MIC is a full language (it can run shell commands, read and
    /// write files, and fetch URLs), so a command from outside, where a prompt could have written it, must be
    /// filter names from the catalogue or a short list of built-ins, each followed only by numbers; no
    /// strings, paths, substitutions or definitions. COMPOSITOR_GMIC_UNRESTRICTED=1 lifts the check.
    static bool allowedForAutomation(const QString& command, QString* why);
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
    // The in-process path: a worker thread, an abort flag the interpreter polls, and a run number so a
    // cancelled run's result is dropped.
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> abort_;
    uint64_t run_ = 0;
};

} // namespace app
