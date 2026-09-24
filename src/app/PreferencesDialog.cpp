#include "Style.h"
#include "PreferencesDialog.h"
#include "Automation.h"
#include "Theme.h"
#include <QSettings>
#include <QSpinBox>
#include "Autosave.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
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

    auto* recovery = new QGroupBox(tr("Crash recovery"));
    auto* recoveryRow = new QHBoxLayout(recovery);
    recoveryRow->addWidget(new QLabel(tr("Autosave every")));
    auto* minutes = new QSpinBox;
    minutes->setRange(0, 120);
    minutes->setSuffix(tr(" min"));
    minutes->setSpecialValueText(tr("Off"));
    minutes->setValue(Autosave::intervalMinutes());
    connect(minutes, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int v) { Autosave::setIntervalMinutes(v); });
    recoveryRow->addWidget(minutes);
    auto* recoveryHint = new QLabel(tr("Unsaved changes are kept aside in the background, and offered back if the editor quits unexpectedly. Your files are not touched."));
    recoveryHint->setWordWrap(true);
    recoveryHint->setStyleSheet(hintStyle());
    recoveryRow->addWidget(recoveryHint, 1);
    layout->addWidget(recovery);

    auto* automation = new QGroupBox(tr("Automation"));
    auto* av = new QVBoxLayout(automation);
    auto* rpc = new QCheckBox(tr("Listen for agents on the automation socket at startup"));
    rpc->setChecked(QSettings().value("automation/enabled", false).toBool());
    connect(rpc, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("automation/enabled", on); });
    av->addWidget(rpc);
    auto* rpcInfo = new QLabel(tr("Lets an MCP bridge or a script drive the editor over a local socket (%1). "
                                  "Only programs running as you can connect. Takes effect at the next launch; "
                                  "nekophoto --rpc turns it on for one run.").arg(AutomationServer::defaultSocketPath()));
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
