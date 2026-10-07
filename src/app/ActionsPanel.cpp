#include "ActionsPanel.h"
#include "ActionLibrary.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace app {

namespace {

constexpr int nameRole = Qt::UserRole, stepRole = Qt::UserRole + 1;

QToolButton* button(const QString& text, const QString& tip, const QIcon& icon = {}) {
    auto* b = new QToolButton;
    if (icon.isNull()) b->setText(text); else b->setIcon(icon);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

} // namespace

ActionsPanel::ActionsPanel(std::function<QString(const QString&)> play, QWidget* parent) : QWidget(parent), play_(std::move(play)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(1);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setToolTip(tr("Tick a step to include it when the action plays; double-click a step to edit its parameters."));
    layout->addWidget(tree_, 1);
    status_ = new QLabel;
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* row = new QHBoxLayout;
    row->setSpacing(0);
    auto* stop = button(tr("Stop"), tr("Stop recording"), style()->standardIcon(QStyle::SP_MediaStop));
    record_ = button(QStringLiteral("●"), tr("Record: every edit is added to the selected action (or a new one)"));
    record_->setCheckable(true);
    record_->setStyleSheet(QStringLiteral("QToolButton { color: #d03030; font-weight: bold; }"));
    playButton_ = button(tr("Play"), tr("Play the selected action on the current document"), style()->standardIcon(QStyle::SP_MediaPlay));
    auto* add = button(QStringLiteral("+"), tr("New action"));
    auto* remove = button(QStringLiteral("−"), tr("Delete the selected action or step"), style()->standardIcon(QStyle::SP_TrashIcon));
    auto* up = button(tr("Up"), tr("Move the step up"), style()->standardIcon(QStyle::SP_ArrowUp));
    auto* down = button(tr("Down"), tr("Move the step down"), style()->standardIcon(QStyle::SP_ArrowDown));
    auto* more = button(QStringLiteral("…"), tr("Import, export, rename and batch"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->addAction(tr("Rename Action…"), this, [this] {
        const QString name = selectedAction();
        if (name.isEmpty()) return;
        bool ok = false;
        const QString to = QInputDialog::getText(this, tr("Rename Action"), tr("Name"), QLineEdit::Normal, name, &ok);
        if (ok && !ActionLibrary::instance().rename(name, to)) QMessageBox::warning(this, tr("Rename Action"), tr("That name is empty or already used."));
    });
    menu->addAction(tr("Edit Step…"), this, [this] { editStep(); });
    menu->addSeparator();
    menu->addAction(tr("Import Actions…"), this, [this] { importActions(); });
    menu->addAction(tr("Export Actions…"), this, [this] { exportActions(); });
    menu->addSeparator();
    menu->addAction(tr("Batch…"), this, [this] { emit batchRequested(selectedAction()); });
    more->setMenu(menu);
    for (QToolButton* b : {stop, record_, playButton_}) row->addWidget(b);
    row->addStretch(1);
    for (QToolButton* b : {up, down, add, remove, more}) row->addWidget(b);
    layout->addLayout(row);

    connect(stop, &QToolButton::clicked, this, [] { ActionLibrary::instance().stopRecording(); });
    connect(record_, &QToolButton::clicked, this, [this] { toggleRecording(); });
    connect(playButton_, &QToolButton::clicked, this, [this] { playSelected(); });
    connect(add, &QToolButton::clicked, this, [this] { newAction(); });
    connect(remove, &QToolButton::clicked, this, [this] { deleteSelected(); });
    connect(up, &QToolButton::clicked, this, [this] { moveStep(-1); });
    connect(down, &QToolButton::clicked, this, [this] { moveStep(1); });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { if (item && item->parent()) editStep(); });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
        // A step's checkbox: whether it plays.
        if (rebuilding_ || !item || !item->parent()) return;
        const QString name = item->data(0, nameRole).toString();
        const int step = item->data(0, stepRole).toInt();
        const RecordedAction* found = ActionLibrary::instance().find(name);
        if (!found || step < 0 || step >= int(found->steps.size())) return;
        RecordedAction copy = *found;
        copy.steps[size_t(step)].enabled = item->checkState(0) == Qt::Checked;
        ActionLibrary::instance().put(copy);
    });
    ActionLibrary& library = ActionLibrary::instance();
    connect(&library, &ActionLibrary::changed, this, [this] { rebuild(); });
    connect(&library, &ActionLibrary::recordingChanged, this, [this](bool on) {
        record_->setChecked(on);
        status_->setText(on ? tr("Recording “%1”").arg(ActionLibrary::instance().recordingName()) : QString());
        rebuild();
    });
    rebuild();
}

void ActionsPanel::rebuild() {
    int step = -1;
    const QString keep = selectedAction(&step);
    rebuilding_ = true;
    tree_->clear();
    QTreeWidgetItem* select = nullptr;
    ActionLibrary& library = ActionLibrary::instance();
    for (const RecordedAction& a : library.actions()) {
        auto* item = new QTreeWidgetItem(tree_, {a.name});
        item->setData(0, nameRole, a.name);
        item->setData(0, stepRole, -1);
        if (library.recordingName() == a.name) { QFont f = item->font(0); f.setBold(true); item->setFont(0, f); item->setText(0, a.name + tr("  (recording)")); }
        if (a.name == keep && step < 0) select = item;
        for (size_t i = 0; i < a.steps.size(); i++) {
            auto* child = new QTreeWidgetItem(item, {ActionLibrary::describe(a.steps[i])});
            child->setData(0, nameRole, a.name);
            child->setData(0, stepRole, int(i));
            child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            child->setCheckState(0, a.steps[i].enabled ? Qt::Checked : Qt::Unchecked);
            child->setToolTip(0, QString::fromUtf8(QJsonDocument(ActionLibrary::toJson({a.name, {a.steps[i]}}).value("steps").toArray().first().toObject()).toJson(QJsonDocument::Indented)));
            if (a.name == keep && int(i) == step) select = child;
        }
        if (a.name == keep || library.recordingName() == a.name) item->setExpanded(true);
    }
    if (select) tree_->setCurrentItem(select);
    else if (tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0));
    rebuilding_ = false;
    if (tree_->topLevelItemCount() == 0) status_->setText(library.recording() ? status_->text() : tr("No actions yet: press + or ● to record one."));
    else if (!library.recording()) status_->clear();
}

QString ActionsPanel::selectedAction(int* step) const {
    QTreeWidgetItem* item = tree_->currentItem();
    if (step) *step = item ? item->data(0, stepRole).toInt() : -1;
    return item ? item->data(0, nameRole).toString() : QString();
}

void ActionsPanel::playSelected() {
    const QString name = selectedAction();
    if (name.isEmpty()) return;
    if (ActionLibrary::instance().recording()) { QMessageBox::information(this, tr("Play Action"), tr("Stop recording first.")); return; }
    const QString error = play_(name);
    status_->setText(error.isEmpty() ? tr("Played “%1”.").arg(name) : error);
}

void ActionsPanel::toggleRecording() {
    ActionLibrary& library = ActionLibrary::instance();
    if (library.recording()) { library.stopRecording(); return; }
    QString name = selectedAction();
    if (name.isEmpty()) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("New Action"), tr("Name"), QLineEdit::Normal, library.uniqueName(tr("Action")), &ok);
        if (!ok || name.trimmed().isEmpty()) { record_->setChecked(false); return; }
    }
    library.startRecording(name);
}

void ActionsPanel::newAction() {
    ActionLibrary& library = ActionLibrary::instance();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Action"), tr("Name"), QLineEdit::Normal, library.uniqueName(tr("Action")), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    if (library.find(name.trimmed())) { QMessageBox::warning(this, tr("New Action"), tr("There is an action with that name already.")); return; }
    if (library.recording()) library.stopRecording();
    library.startRecording(name.trimmed());   // Photoshop starts recording a new action straight away
}

void ActionsPanel::deleteSelected() {
    int step = -1;
    const QString name = selectedAction(&step);
    const RecordedAction* found = ActionLibrary::instance().find(name);
    if (!found) return;
    if (step < 0) {
        if (QMessageBox::question(this, tr("Delete Action"), tr("Delete the action “%1”?").arg(name)) == QMessageBox::Yes) ActionLibrary::instance().remove(name);
        return;
    }
    RecordedAction copy = *found;
    if (step >= int(copy.steps.size())) return;
    copy.steps.erase(copy.steps.begin() + step);
    ActionLibrary::instance().put(copy);
}

void ActionsPanel::moveStep(int by) {
    int step = -1;
    const QString name = selectedAction(&step);
    const RecordedAction* found = ActionLibrary::instance().find(name);
    if (!found || step < 0 || step + by < 0 || step + by >= int(found->steps.size())) return;
    RecordedAction copy = *found;
    std::swap(copy.steps[size_t(step)], copy.steps[size_t(step + by)]);
    ActionLibrary::instance().put(copy);
    // Keep the moved step selected.
    for (int i = 0; i < tree_->topLevelItemCount(); i++) {
        QTreeWidgetItem* top = tree_->topLevelItem(i);
        if (top->data(0, nameRole).toString() == name && step + by < top->childCount()) tree_->setCurrentItem(top->child(step + by));
    }
}

void ActionsPanel::editStep() {
    int step = -1;
    const QString name = selectedAction(&step);
    const RecordedAction* found = ActionLibrary::instance().find(name);
    if (!found || step < 0 || step >= int(found->steps.size())) return;
    const ActionStep& s = found->steps[size_t(step)];
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Edit Step: %1").arg(s.method));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("The parameters, as the automation method %1 takes them (rpc.describe lists them):").arg(s.method)));
    auto* text = new QPlainTextEdit(QString::fromUtf8(QJsonDocument(s.params).toJson(QJsonDocument::Indented)));
    text->setMinimumSize(420, 260);
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(text->toPlainText().toUtf8(), &error);
    if (!doc.isObject()) { QMessageBox::warning(this, dialog.windowTitle(), tr("That is not a JSON object: %1").arg(error.errorString())); return; }
    if (const QString why = ActionLibrary::stepRefusal(s.method, doc.object()); !why.isEmpty()) { QMessageBox::warning(this, dialog.windowTitle(), why); return; }
    RecordedAction copy = *found;
    copy.steps[size_t(step)].params = doc.object();
    ActionLibrary::instance().put(copy);
}

void ActionsPanel::importActions() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Actions"), QSettings().value("lastDir").toString(), tr("Actions (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    const QStringList names = ActionLibrary::instance().importFile(path, &error);
    if (names.isEmpty()) QMessageBox::warning(this, tr("Import Actions"), tr("Couldn’t import the actions: %1").arg(error));
    else status_->setText(tr("Imported %1.").arg(names.join(", ")));
}

void ActionsPanel::exportActions() {
    const QString name = selectedAction();
    QStringList names;
    if (!name.isEmpty()) names << name;
    else for (const RecordedAction& a : ActionLibrary::instance().actions()) names << a.name;
    if (names.isEmpty()) return;
    QString path = QFileDialog::getSaveFileName(this, tr("Export Actions"), QSettings().value("lastDir").toString() + "/" + names.first() + ".json", tr("Actions (*.json)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";
    QString error;
    if (!ActionLibrary::instance().exportFile(names, path, &error)) QMessageBox::warning(this, tr("Export Actions"), error);
}

} // namespace app
