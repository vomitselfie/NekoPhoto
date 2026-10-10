#include "Style.h"
#include "PreferencesDialog.h"
#include "ColorManagement.h"
#include "ExportAs.h"
#include "Automation.h"
#include "Theme.h"
#include "CpuPower.h"
#include "Language.h"
#include "Gmic.h"
#include "GmicStore.h"
#include <QSettings>
#include <QSpinBox>
#include "Autosave.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QTimer>
#include <QVBoxLayout>

namespace app {

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Preferences"));
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(this);
    // Word-wrapped hint labels report a one-line minimum, so a dialog sized from minimums opens shorter than
    // its contents and the rows draw over each other; once shown it has a width, and the height for that width
    // becomes its minimum.
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    QTimer::singleShot(0, this, [this, layout] {
        const int height = layout->totalHeightForWidth(width());
        if (height > minimumHeight()) { setMinimumHeight(height); resize(width(), height); }
    });

    auto* appearance = new QGroupBox(tr("Appearance"));
    auto* appearanceRow = new QHBoxLayout(appearance);
    appearanceRow->addWidget(new QLabel(tr("Theme")));
    auto* theme = new QComboBox;
    theme->addItem(tr("Goth Kitty"), "gothkitty");
    theme->addItem(tr("System"), "system");
    theme->addItem(tr("Dark"), "dark");
    theme->addItem(tr("Light"), "light");
    theme->setCurrentIndex(std::max(0, theme->findData(themeSetting())));
    connect(theme, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [theme](int) { setThemeSetting(theme->currentData().toString()); applyTheme(); });
    appearanceRow->addWidget(theme, 1);
    auto* themeHint = new QLabel(tr("System follows the desktop (through its portal when running as an AppImage). Some text colours refresh at the next launch."));
    themeHint->setWordWrap(true);
    themeHint->setStyleSheet(hintStyle());
    appearanceRow->addWidget(themeHint, 2);
    layout->addWidget(appearance);

    auto* languageBox = new QGroupBox(tr("Language"));
    auto* languageRow = new QHBoxLayout(languageBox);
    languageRow->addWidget(new QLabel(tr("Language")));
    auto* language = new QComboBox;
    language->addItem(tr("System default"), "system");
    for (const QString& code : language::available()) language->addItem(language::nativeName(code), code);
    language->setCurrentIndex(std::max(0, language->findData(language::setting())));
    connect(language, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [language](int) { language::setSetting(language->currentData().toString()); });
    languageRow->addWidget(language, 1);
    auto* languageHint = new QLabel(tr("Takes effect the next time NekoPhoto starts."));
    languageHint->setWordWrap(true);
    languageHint->setStyleSheet(hintStyle());
    languageRow->addWidget(languageHint, 2);
    layout->addWidget(languageBox);

    // CPU power: how much of the machine NekoPhoto takes, for when something else (a render) should come first.
    auto* performanceBox = new QGroupBox(tr("Performance"));
    auto* performanceGrid = new QGridLayout(performanceBox);
    performanceGrid->addWidget(new QLabel(tr("CPU power")), 0, 0);
    auto* cpu = new QComboBox;
    for (const QString& level : cpupower::levels()) cpu->addItem(cpupower::label(level), level);
    cpu->setCurrentIndex(std::max(0, cpu->findData(cpupower::setting())));
    performanceGrid->addWidget(cpu, 0, 1);
    auto* cpuHint = new QLabel;
    cpuHint->setWordWrap(true);
    cpuHint->setStyleSheet(hintStyle());
    performanceGrid->addWidget(cpuHint, 1, 0, 1, 2);
    auto showCpu = [cpu, cpuHint] {
        const QString level = cpu->currentData().toString();
        QString text = cpupower::describe(level);
        // The thread pools are sized at launch: say so whatever is chosen, not only once it differs.
        text += QStringLiteral(" ") + tr("Takes effect the next time NekoPhoto starts.");
        cpuHint->setText(text);
    };
    connect(cpu, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [cpu, showCpu](int) { cpupower::setSetting(cpu->currentData().toString()); showCpu(); });
    showCpu();
    performanceGrid->setColumnStretch(1, 1);
    layout->addWidget(performanceBox);

    auto* group = new QGroupBox(tr("AI background removal"));
    auto* v = new QVBoxLayout(group);
    enable_ = new QCheckBox(tr("Enable Filter > Remove Background"));
    enable_->setChecked(ModelStore::enabled());
    enable_->setEnabled(ModelStore::supported());
    v->addWidget(enable_);
    auto* intro = new QLabel(ModelStore::supported()
        ? tr("Finds the subject of a layer with a segmentation model that runs on this computer; nothing is sent anywhere. "
             "The model is a separate download from the rembg project (Apache-2.0), kept in the folder below.")
        : tr("This build was made without OpenCV, which runs the segmentation model, so the feature is unavailable."));
    intro->setWordWrap(true);
    intro->setStyleSheet(hintStyle());
    v->addWidget(intro);

    auto* modelRow = new QHBoxLayout;
    modelRow->addWidget(new QLabel(tr("Model")));
    model_ = new QComboBox;
    for (auto& m : ModelStore::models()) if (!m.prompt) model_->addItem(QStringLiteral("%1 (%2 MB)").arg(m.label).arg(m.bytes / 1e6, 0, 'f', m.bytes > 50e6 ? 0 : 1), m.id);
    model_->setCurrentIndex(std::max(0, model_->findData(ModelStore::selected().id)));
    modelRow->addWidget(model_, 1);
    v->addLayout(modelRow);
    auto* mirror = new QCheckBox(tr("Average with the mirrored image (steadier edges, twice the model time)"));
    mirror->setToolTip(tr("The model runs on the image and on its mirror and the two masks are averaged. Measured on AIM-500 this lowers the error on most subjects, portraits and furniture most of all."));
    mirror->setChecked(ModelStore::mirrorAverage());
    connect(mirror, &QCheckBox::toggled, this, [](bool on) { ModelStore::setMirrorAverage(on); });
    v->addWidget(mirror);
    about_ = new QLabel;
    about_->setWordWrap(true);
    about_->setStyleSheet(hintStyle());
    v->addWidget(about_);

    auto* statusRow = new QHBoxLayout;
    status_ = new QLabel;
    statusRow->addWidget(status_, 1);
    download_ = new QPushButton(tr("Download"));
    remove_ = new QPushButton(tr("Remove"));
    cancel_ = new QPushButton(tr("Cancel"));
    statusRow->addWidget(download_);
    statusRow->addWidget(remove_);
    statusRow->addWidget(cancel_);
    v->addLayout(statusRow);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setVisible(false);
    v->addWidget(progress_);

    auto* locationRow = new QHBoxLayout;
    location_ = new QLabel;
    location_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    location_->setWordWrap(true);
    locationRow->addWidget(location_, 1);
    auto* open = new QPushButton(tr("Show Folder"));
    connect(open, &QPushButton::clicked, this, [] { QDir().mkpath(ModelStore::directory()); QDesktopServices::openUrl(QUrl::fromLocalFile(ModelStore::directory())); });
    locationRow->addWidget(open);
    v->addLayout(locationRow);
    layout->addWidget(group);

    // G'MIC: where it was found; on Windows it can be downloaded from gmic.eu (GmicStore).
    auto* gmicBox = new QGroupBox(tr("G'MIC"));
    gmicBox->setObjectName("gmicPreferences");
    auto* gv = new QVBoxLayout(gmicBox);
    auto* gmicRow = new QHBoxLayout;
    gmicStatus_ = new QLabel;
    gmicStatus_->setWordWrap(true);
    gmicStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gmicRow->addWidget(gmicStatus_, 1);
    gmicDownload_ = new QPushButton(tr("Download"));
    gmicRemove_ = new QPushButton(tr("Remove"));
    gmicCancel_ = new QPushButton(tr("Cancel"));
    auto* gmicFolder = new QPushButton(tr("Show Folder"));
    gmicRow->addWidget(gmicDownload_);
    gmicRow->addWidget(gmicRemove_);
    gmicRow->addWidget(gmicCancel_);
    gmicRow->addWidget(gmicFolder);
    gv->addLayout(gmicRow);
    gmicProgress_ = new QProgressBar;
    gmicProgress_->setRange(0, 1000);
    gmicProgress_->setVisible(false);
    gv->addWidget(gmicProgress_);
    auto* gmicHint = new QLabel(GmicStore::offered()
        ? tr("Filter > G'MIC runs G'MIC, free software from gmic.eu under the CeCILL 2.1 licence. NekoPhoto does not include it: "
             "Download fetches the command-line G'MIC %1 for Windows (%2 MB) from gmic.eu into %3.")
              .arg(GmicStore::pinned().version)
              .arg(qRound(GmicStore::pinned().bytes / 1e6))
              .arg(QDir::toNativeSeparators(GmicStore::directory()))
        : tr("Filter > G'MIC runs G'MIC, free software from gmic.eu under the CeCILL 2.1 licence. "
             "Install G'MIC from your package manager (gmic)."));
    gmicHint->setWordWrap(true);
    gmicHint->setStyleSheet(hintStyle());
    gv->addWidget(gmicHint);
    layout->addWidget(gmicBox);
    gmicStore_ = new GmicStore(this);
    connect(gmicDownload_, &QPushButton::clicked, this, [this] {
        gmicProgress_->setValue(0);
        gmicStore_->download();
        syncGmic();
    });
    connect(gmicCancel_, &QPushButton::clicked, gmicStore_, &GmicStore::cancel);
    connect(gmicRemove_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Remove G'MIC?"), tr("Delete the downloaded G'MIC from %1? It can be downloaded again later.")
                                      .arg(QDir::toNativeSeparators(GmicRunner::downloadedDirectory()))) != QMessageBox::Yes) return;
        if (QString error; !GmicStore::remove(&error)) QMessageBox::warning(this, tr("G'MIC"), error);
        syncGmic();
    });
    connect(gmicFolder, &QPushButton::clicked, this, [] {
        const GmicRunner::Location found = GmicRunner::locate();
        const QString folder = found.path.isEmpty() ? GmicStore::directory() : QFileInfo(found.path).absolutePath();
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    connect(gmicStore_, &GmicStore::progress, this, [this](qint64 received, qint64 total) { gmicProgress_->setValue(int(received * 1000 / std::max<qint64>(1, total))); });
    connect(gmicStore_, &GmicStore::succeeded, this, [this] { syncGmic(); });
    connect(gmicStore_, &GmicStore::cancelled, this, [this] { syncGmic(); gmicStatus_->setText(tr("Download cancelled.")); });
    connect(gmicStore_, &GmicStore::failed, this, [this](const QString& error) { syncGmic(); QMessageBox::warning(this, tr("Download failed"), error); });
    syncGmic();

    // File > Export > Quick Export's format, written with the settings Export As last used for it.
    auto* exportBox = new QGroupBox(tr("Export"));
    auto* exportRow = new QHBoxLayout(exportBox);
    exportRow->addWidget(new QLabel(tr("Quick Export format")));
    auto* quickFormat = new QComboBox;
    quickFormat->setObjectName("quickExportFormat");
    for (const QString& f : exportas::formats()) quickFormat->addItem(exportas::formatLabel(f), f);
    quickFormat->setCurrentIndex(std::max(0, quickFormat->findData(exportas::quickExportFormat())));
    connect(quickFormat, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [quickFormat](int) { exportas::setQuickExportFormat(quickFormat->currentData().toString()); });
    exportRow->addWidget(quickFormat, 1);
    auto* exportHint = new QLabel(tr("Quick Export writes beside the document with the settings File > Export > Export As last used for the format."));
    exportHint->setWordWrap(true);
    exportHint->setStyleSheet(hintStyle());
    exportRow->addWidget(exportHint, 2);
    layout->addWidget(exportBox);

    auto* recovery = new QGroupBox(tr("Crash recovery"));
    auto* recoveryRow = new QHBoxLayout(recovery);
    recoveryRow->addWidget(new QLabel(tr("Autosave every")));
    auto* minutes = new QSpinBox;
    minutes->setObjectName("autosaveMinutes");
    minutes->setRange(0, 120);
    minutes->setSuffix(tr(" min"));
    minutes->setSpecialValueText(tr("Off"));
    minutes->setValue(Autosave::intervalMinutes());
    // The new interval applies at once (and turning it on from Off starts it), not when the dialog closes.
    connect(minutes, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { Autosave::setIntervalMinutes(v); emit autosaveIntervalChanged(v); });
    recoveryRow->addWidget(minutes);
    auto* recoveryHint = new QLabel(tr("Unsaved changes are kept aside in the background, and offered back if the editor quits unexpectedly. Your files are not touched."));
    recoveryHint->setWordWrap(true);
    recoveryHint->setStyleSheet(hintStyle());
    recoveryRow->addWidget(recoveryHint, 1);
    layout->addWidget(recovery);

    layout->addWidget(color::monitorPreferences(this));   // the canvas's colour transform (ColorManagement.h)

    auto* automation = new QGroupBox(tr("Automation"));
    auto* av = new QVBoxLayout(automation);
    auto* rpc = new QCheckBox(tr("Listen for agents on the automation socket at startup"));
    rpc->setChecked(QSettings().value("automation/enabled", false).toBool());
    connect(rpc, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("automation/enabled", on); });
    av->addWidget(rpc);
    auto* rpcInfo = new QLabel(tr("Lets an MCP bridge or a script drive the editor over a local socket (%1). "
                                  "Only programs running as you can connect; nekophoto --rpc turns it on for one run.").arg(AutomationServer::defaultSocketPath())
                               + QStringLiteral(" ") + tr("Takes effect the next time NekoPhoto starts."));
    rpcInfo->setWordWrap(true);
    rpcInfo->setStyleSheet(hintStyle());
    av->addWidget(rpcInfo);
    layout->addWidget(automation);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(enable_, &QCheckBox::toggled, this, [this](bool on) {
        ModelStore::setEnabled(on);
        // Turning it on with no model yet starts the download, which is the whole point of the switch.
        if (on && !ModelStore::isPresent(chosen()) && !active_) startDownload();
        syncStatus();
        emit backgroundRemovalChanged();
    });
    connect(model_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        ModelStore::setSelected(model_->currentData().toString());
        syncStatus();
        emit backgroundRemovalChanged();
    });
    connect(download_, &QPushButton::clicked, this, &PreferencesDialog::startDownload);
    connect(remove_, &QPushButton::clicked, this, &PreferencesDialog::removeModel);
    connect(cancel_, &QPushButton::clicked, this, [this] { if (active_) active_->cancel(); });
    syncStatus();
}

PreferencesDialog::~PreferencesDialog() { if (active_) active_->cancel(); }

const ModelInfo& PreferencesDialog::chosen() const {
    const ModelInfo* m = ModelStore::modelById(model_->currentData().toString());
    return m ? *m : ModelStore::models().front();
}

void PreferencesDialog::syncStatus() {
    const ModelInfo& m = chosen();
    about_->setText(m.about);
    bool present = ModelStore::isPresent(m), busy = active_.has_value(), supported = ModelStore::supported();
    if (busy) status_->setText(tr("Downloading %1…").arg(m.label));
    else if (present) status_->setText(tr("Downloaded and ready."));
    else status_->setText(tr("Not downloaded (%1 MB).").arg(m.bytes / 1e6, 0, 'f', m.bytes > 50e6 ? 0 : 1));
    download_->setVisible(!present && !busy);
    download_->setEnabled(supported);
    remove_->setVisible(present && !busy);
    cancel_->setVisible(busy);
    progress_->setVisible(busy);
    model_->setEnabled(!busy);
    location_->setText(tr("Models folder: %1").arg(ModelStore::directory()));
}

void PreferencesDialog::syncGmic() {
    const bool busy = gmicStore_->busy(), downloaded = GmicStore::downloaded();
    const GmicRunner::Location found = GmicRunner::locate();
    if (busy) gmicStatus_->setText(tr("Downloading G'MIC %1…").arg(GmicStore::pinned().version));
    else if (found.source == QLatin1String("downloaded")) gmicStatus_->setText(tr("Downloaded G'MIC %1, in %2.").arg(GmicRunner::version(), QDir::toNativeSeparators(QFileInfo(found.path).absolutePath())));
    else if (!found.path.isEmpty()) {
        const QString where = found.source == QLatin1String("env") ? tr("set by COMPOSITOR_GMIC")
                            : found.source == QLatin1String("beside") ? tr("beside NekoPhoto")
                                                                      : tr("on PATH");
        gmicStatus_->setText(tr("G'MIC %1 found at %2 (%3).").arg(GmicRunner::version(), QDir::toNativeSeparators(found.path), where));
    } else if (GmicRunner::available()) gmicStatus_->setText(tr("G'MIC %1 runs inside NekoPhoto (libgmic).").arg(GmicRunner::version()));
    else gmicStatus_->setText(tr("G'MIC is not installed."));
    gmicDownload_->setVisible(GmicStore::offered() && !downloaded && found.path.isEmpty() && !busy);
    gmicRemove_->setVisible(downloaded && !busy);
    gmicCancel_->setVisible(busy);
    gmicProgress_->setVisible(busy);
}

void PreferencesDialog::startDownload() {
    if (active_) return;
    const ModelInfo& m = chosen();
    progress_->setValue(0);
    active_ = ModelStore::download(m, this,
        [this](qint64 received, qint64 total) { progress_->setValue(int(received * 1000 / std::max<qint64>(1, total))); },
        [this](QString path, QString error) {
            active_.reset();
            if (!error.isEmpty()) QMessageBox::warning(this, tr("Download failed"), error);
            else if (path.isEmpty()) status_->setText(tr("Download cancelled."));
            syncStatus();
            emit backgroundRemovalChanged();
        });
    syncStatus();
}

void PreferencesDialog::removeModel() {
    const ModelInfo& m = chosen();
    if (QMessageBox::question(this, tr("Remove the model?"), tr("Delete %1 from disk? It can be downloaded again later.").arg(m.name)) != QMessageBox::Yes) return;
    ModelStore::remove(m);
    syncStatus();
    emit backgroundRemovalChanged();
}

} // namespace app
