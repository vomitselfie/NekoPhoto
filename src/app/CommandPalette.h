// Edit > Search… (Ctrl+F, Photoshop's key): a popup that finds any menu command, tool or G'MIC filter by a fuzzy
// match on its name, its English name or its menu path, and runs it.
#pragma once
#include <QFrame>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
#include <vector>

class QAction;
class QLineEdit;
class QTreeWidget;

namespace app {

/// One thing the palette can run.
struct PaletteEntry {
    QString label;          // as the interface shows it
    QString path;           // the menu path, "Filter > Blur" (translated)
    QString englishLabel;   // the source text, matched as well as the translation
    QString englishPath;
    QString shortcut;
    QString id;             // stable across languages: kind and English path, for Recently Used
    QPointer<QAction> action;          // runs it when set (its enabled state is the entry's)
    std::function<void()> run;         // otherwise this
    bool enabled = true;
    bool isEnabled() const;
};

/// Case-insensitive subsequence score, higher is better; nullopt when `query` is not a subsequence of `text`.
/// A run of consecutive characters and a character at a word's start score more; long texts score a little less.
std::optional<int> fuzzyScore(const QString& query, const QString& text);

/// The best score of an entry over its label, its English label and its paths.
std::optional<int> paletteScore(const QString& query, const PaletteEntry& entry);

/// The English source of a translated interface string (with the menu accelerators dropped), or the string itself.
QString englishText(const QString& translated);

/// The interface string without its accelerator: "&File" and "ファイル(&F)" become "File" and "ファイル".
QString plainText(const QString& text);

class CommandPalette : public QFrame {
    Q_OBJECT
public:
    CommandPalette(std::vector<PaletteEntry> entries, QWidget* parent);
    void setQuery(const QString& query);
    /// The rows shown, best first (for the self-test).
    QStringList resultLabels() const;
    /// Runs the current row, if it is enabled; the palette closes.
    void runCurrent();

    static QStringList recent();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refill();
    void moveCurrent(int by);

    std::vector<PaletteEntry> entries_;
    std::vector<int> shown_;
    QLineEdit* field_;
    QTreeWidget* list_;
};

} // namespace app
