// Actions (Photoshop's Actions panel): named sequences of automation calls, recorded as the person works and
// played back on the current document or a folder of files (File > Automate > Batch). A step is exactly an
// automation request ({"method", "params"}, see docs/automation.md), so anything the socket can do can be
// played, edited as JSON, imported and exported. The library lives in the app data folder (actions.json) so it
// outlasts the session. What records: every editing request that reaches the automation socket while recording,
// and the menu commands, dialogs and tools that call recordAction with the request that repeats them (see
// docs/actions.md for the list).
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

namespace app {

struct ActionStep {
    QString method;
    QJsonObject params;
    bool enabled = true;
};

struct RecordedAction {
    QString name;
    std::vector<ActionStep> steps;
};

class ActionLibrary : public QObject {
    Q_OBJECT
public:
    static ActionLibrary& instance();
    /// The library file (created on the first save).
    static QString filePath();

    const std::vector<RecordedAction>& actions();
    const RecordedAction* find(const QString& name);
    /// Adds the action, or replaces the one with its name.
    void put(const RecordedAction& action);
    bool remove(const QString& name);
    bool rename(const QString& from, const QString& to);
    /// A name not yet used: `base`, then "base 2", ...
    QString uniqueName(const QString& base);

    static QJsonObject toJson(const RecordedAction& action);
    static std::optional<RecordedAction> fromJson(const QJsonObject& json, QString* error);
    /// A step as one line for the panel: "Gaussian Blur  radius 4" and the like.
    static QString describe(const ActionStep& step);
    /// Imports a file written by exportFile (one action, or {"actions": [...]}); returns the names added.
    QStringList importFile(const QString& path, QString* error);
    bool exportFile(const QStringList& names, const QString& path, QString* error);

    // Recording
    bool recording() const { return !recordingName_.isEmpty(); }
    QString recordingName() const { return recordingName_; }
    /// Starts recording into `name` (appending to it when it exists).
    void startRecording(const QString& name);
    /// Stops; returns how many steps this recording added.
    int stopRecording();
    /// Adds a step to the action being recorded, unless nothing is recording or a playback or automation request
    /// is running (their own steps are recorded once, at the top).
    void record(const QString& method, const QJsonObject& params = {});
    /// Whether a method changes the document (worth recording) rather than looking at it or the app.
    static bool recordable(const QString& method);
    /// While one lives, record() does nothing.
    struct Quiet {
        Quiet() { instance().quiet_++; }
        ~Quiet() { instance().quiet_--; }
        Quiet(const Quiet&) = delete;
        Quiet& operator=(const Quiet&) = delete;
    };
    bool quiet() const { return quiet_ > 0; }

signals:
    void changed();
    void recordingChanged(bool recording);

private:
    ActionLibrary() = default;
    void load();
    void save();
    bool loaded_ = false;
    std::vector<RecordedAction> actions_;
    QString recordingName_;
    int recordedSteps_ = 0;
    int quiet_ = 0;
};

/// Records a step if an action is recording (see ActionLibrary::record).
inline void recordAction(const QString& method, const QJsonObject& params = {}) { ActionLibrary::instance().record(method, params); }

} // namespace app
