// Edit > Search…: see CommandPalette.h. The fuzzy score is adapted from PhotoCraft's command palette
// (THIRD-PARTY-NOTICES.md).
#include "CommandPalette.h"
#include "Language.h"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <algorithm>

namespace app {

namespace {

constexpr int kRecentCount = 8;
constexpr int kMaxRows = 80;
const char* kRecentKey = "search/recent";

bool wordStart(const QString& text, qsizetype i) {
    if (i == 0) return true;
    const QChar before = text[i - 1];
    return before.isSpace() || before == '>' || before == '/' || before == '(' || before == '-' || before == '_' || before == '.' || before == '\'';
}

} // namespace

bool PaletteEntry::isEnabled() const { return action ? action->isEnabled() : enabled; }

std::optional<int> fuzzyScore(const QString& query, const QString& text) {
    if (query.isEmpty()) return 0;
    const QString t = text.toLower();
    const QString q = query.toLower();
    int score = 0;
    qsizetype ti = 0, last = -2;
    std::vector<qsizetype> at;
    for (const QChar qc : q) {
        bool found = false;
        while (ti < t.size()) {
            if (t[ti] == qc) {
                score += last == ti - 1 ? 8 : 1;   // a contiguous run
                if (wordStart(t, ti)) score += 5;
                at.push_back(ti);
                last = ti++;
                found = true;
                break;
            }
            ti++;
        }
        if (!found) return std::nullopt;
    }
    // Every letter starts a word or sits in a run of two or more: a lone letter picked out of the middle of a word
    // ("gauss" spelled across "Iain Fergusson") is not a match at all.
    for (size_t k = 0; k < at.size(); k++) {
        if (q[qsizetype(k)].isSpace() || wordStart(t, at[k])) continue;
        const bool runBefore = k > 0 && at[k - 1] == at[k] - 1, runAfter = k + 1 < at.size() && at[k + 1] == at[k] + 1;
        if (!runBefore && !runAfter) return std::nullopt;
    }
    return score - int(t.size() / 8);
}

std::optional<int> paletteScore(const QString& query, const PaletteEntry& e) {
    std::optional<int> best;
    auto consider = [&](const QString& text, int bonus) {
        if (text.isEmpty()) return;
        if (auto s = fuzzyScore(query, text)) best = std::max(best.value_or(*s + bonus), *s + bonus);
    };
    // The name itself counts most; the path helps a query like "blur gauss" or "sel subj".
    consider(e.label, 6);
    if (e.englishLabel != e.label) consider(e.englishLabel, 6);
    consider(e.label + ' ' + e.path, 0);
    consider(e.path + ' ' + e.label, 0);
    if (e.englishLabel != e.label || e.englishPath != e.path) {
        consider(e.englishLabel + ' ' + e.englishPath, 0);
        consider(e.englishPath + ' ' + e.englishLabel, 0);
    }
    return best;
}

QString plainText(const QString& text) {
    static const QRegularExpression accelerator(QStringLiteral("\\(&[^)]\\)"));
    QString out = text.section('\t', 0, 0);
    out.remove(accelerator);
    out.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    out.remove('&');
    out.replace(QChar(1), QChar('&'));
    return out.trimmed();
}

QString englishText(const QString& translated) {
    // The translation file is embedded beside the compiled one (src/app/CMakeLists.txt); read once, reversed.
    static QHash<QString, QString> reverse;
    static QString loadedFor;
    const QString lang = language::current();
    if (lang.isEmpty() || lang == QLatin1String("en")) return translated;
    if (loadedFor != lang) {
        loadedFor = lang;
        reverse.clear();
        QFile file(QStringLiteral(":/i18n-src/nekophoto_%1.ts").arg(lang));
        if (file.open(QIODevice::ReadOnly)) {
            QXmlStreamReader xml(&file);
            QString source;
            while (!xml.atEnd()) {
                if (!xml.readNextStartElement()) continue;
                if (xml.name() == QLatin1String("source")) source = xml.readElementText();
                else if (xml.name() == QLatin1String("translation")) {
                    const QString target = plainText(xml.readElementText(QXmlStreamReader::IncludeChildElements));
                    if (!target.isEmpty() && !source.isEmpty()) reverse.insert(target, plainText(source));
                }
            }
        }
    }
    return reverse.value(plainText(translated), translated);
}

CommandPalette::CommandPalette(std::vector<PaletteEntry> entries, QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint), entries_(std::move(entries)) {
    setObjectName(QStringLiteral("commandPalette"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    field_ = new QLineEdit;
    field_->setObjectName(QStringLiteral("commandPaletteField"));
    field_->setPlaceholderText(tr("Search commands, tools and filters"));
    field_->setClearButtonEnabled(true);
    field_->installEventFilter(this);
    layout->addWidget(field_);
    list_ = new QTreeWidget;
    list_->setObjectName(QStringLiteral("commandPaletteList"));
    list_->setColumnCount(3);
    list_->setHeaderHidden(true);
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(list_);
    connect(field_, &QLineEdit::textChanged, this, &CommandPalette::refill);
    connect(list_, &QTreeWidget::itemActivated, this, [this] { runCurrent(); });
    connect(list_, &QTreeWidget::itemClicked, this, [this] { runCurrent(); });
    resize(640, 420);
    refill();
}

namespace {
/// Recently Used, kept for the session as well: it still works when the settings file cannot be written.
QStringList& recentCache() {
    static QStringList list = QSettings().value(kRecentKey).toStringList();
    return list;
}
void rememberRecent(const QStringList& ids) {
    recentCache() = ids;
    QSettings().setValue(kRecentKey, ids);
}
} // namespace

QStringList CommandPalette::recent() { return recentCache(); }

void CommandPalette::setQuery(const QString& query) { field_->setText(query); }

QStringList CommandPalette::resultLabels() const {
    QStringList out;
    for (int i : shown_) out << entries_[size_t(i)].label;
    return out;
}

const PaletteEntry* CommandPalette::resultEntry(int row) const {
    return row >= 0 && size_t(row) < shown_.size() ? &entries_[size_t(shown_[size_t(row)])] : nullptr;
}

QString CommandPalette::rowText(int row, int column) const {
    const QTreeWidgetItem* item = list_->topLevelItem(row);
    return item ? item->text(column) : QString();
}

void CommandPalette::refill() {
    const QString query = field_->text().trimmed();
    shown_.clear();
    if (query.isEmpty()) {
        // Recently used first, then everything in menu order.
        std::vector<bool> taken(entries_.size());
        for (const QString& id : recent())
            for (size_t i = 0; i < entries_.size(); i++)
                if (!taken[i] && entries_[i].id == id) { shown_.push_back(int(i)); taken[i] = true; break; }
        for (size_t i = 0; i < entries_.size() && shown_.size() < size_t(kMaxRows); i++) if (!taken[i]) shown_.push_back(int(i));
    } else {
        std::vector<std::pair<int, int>> scored;   // score, index
        for (size_t i = 0; i < entries_.size(); i++)
            if (auto s = paletteScore(query, entries_[i])) scored.emplace_back(*s, int(i));
        std::stable_sort(scored.begin(), scored.end(), [this](const auto& a, const auto& b) {
            const bool ea = entries_[size_t(a.second)].isEnabled(), eb = entries_[size_t(b.second)].isEnabled();
            if (a.first != b.first) return a.first > b.first;
            return ea && !eb;
        });
        for (size_t i = 0; i < scored.size() && i < size_t(kMaxRows); i++) shown_.push_back(scored[i].second);
    }
    list_->clear();
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    const QColor hint = palette().color(QPalette::PlaceholderText);
    int first = -1;
    for (size_t row = 0; row < shown_.size(); row++) {
        const PaletteEntry& e = entries_[size_t(shown_[row])];
        const bool enabled = e.isEnabled();
        // A greyed command says why beside its menu path ("Nothing to undo", "Not available in 32-bit mode").
        const QString why = enabled ? QString() : e.reason.isEmpty() ? tr("Not available now") : e.reason;
        auto* item = new QTreeWidgetItem(list_, {e.label, why.isEmpty() ? e.path : tr("%1 · %2").arg(e.path, why), e.shortcut});
        for (int c = 0; c < 3; c++) item->setForeground(c, enabled ? (c == 0 ? palette().color(QPalette::Text) : hint) : dim);
        if (!enabled) for (int c = 0; c < 3; c++) item->setToolTip(c, why);
        if (!e.commandId.isEmpty()) item->setData(0, Qt::UserRole, e.commandId);
        if (enabled && first < 0) first = int(row);
    }
    if (list_->topLevelItemCount() > 0) list_->setCurrentItem(list_->topLevelItem(std::max(first, 0)));
}

void CommandPalette::moveCurrent(int by) {
    const int count = list_->topLevelItemCount();
    if (count == 0) return;
    const int row = std::clamp(list_->indexOfTopLevelItem(list_->currentItem()) + by, 0, count - 1);
    list_->setCurrentItem(list_->topLevelItem(row));
    list_->scrollToItem(list_->currentItem());
}

void CommandPalette::runCurrent() {
    const int row = list_->indexOfTopLevelItem(list_->currentItem());
    if (row < 0 || size_t(row) >= shown_.size()) return;
    const PaletteEntry& e = entries_[size_t(shown_[size_t(row)])];
    if (!e.isEnabled()) return;
    QStringList ids = recent();
    ids.removeAll(e.id);
    ids.prepend(e.id);
    while (ids.size() > kRecentCount) ids.removeLast();
    rememberRecent(ids);
    // Run once the popup is gone, so a dialog the command opens is not parented to a closing popup.
    QPointer<QAction> action = e.action;
    std::function<void()> run = e.run;
    close();
    QTimer::singleShot(0, qApp, [action, run] {
        if (action) { if (action->isEnabled()) action->trigger(); }
        else if (run) run();
    });
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == field_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Down: moveCurrent(1); return true;
        case Qt::Key_Up: moveCurrent(-1); return true;
        case Qt::Key_PageDown: moveCurrent(10); return true;
        case Qt::Key_PageUp: moveCurrent(-10); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: runCurrent(); return true;
        case Qt::Key_Escape: close(); return true;
        default: break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace app
