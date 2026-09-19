#include "GmicDialog.h"
#include "Style.h"
#include "compositor/filters.h"
#include "compositor/render.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>

using namespace compositor;

namespace app {

namespace {

constexpr int previewLimit = 1024;

std::shared_ptr<const Image> previewCopy(const std::shared_ptr<const Image>& source, int limit, double& scale) {
    int longest = std::max(source->width(), source->height());
    if (longest <= limit) { scale = 1; return source; }
    scale = double(limit) / longest;
    int w = std::max(1, int(source->width() * scale)), h = std::max(1, int(source->height() * scale));
    LayerTransform full(Point(0, 0), Size(source->width(), source->height()));
    return resampleLayer(source, full, full, w, h);
}

std::shared_ptr<GrayImage> coverageCopy(const std::shared_ptr<GrayImage>& coverage, int w, int h) {
    if (!coverage) return nullptr;
    LayerTransform full(Point(0, 0), Size(coverage->width(), coverage->height()));
    return resampleMask(*coverage, full, full, w, h, 0);
}

GmicParam number(const QString& label, double value, double min, double max, int decimals) {
    GmicParam p;
    p.kind = decimals == 0 ? GmicParam::Int : GmicParam::Float;
    p.label = label; p.value = value; p.min = min; p.max = max; p.decimals = decimals;
    return p;
}

/// A few filters from G'MIC's core, always available even without the catalogue file.
std::vector<GmicFilter> builtinPresets() {
    std::vector<GmicFilter> list;
    auto add = [&](const QString& name, const QString& command, std::vector<GmicParam> params = {}) {
        GmicFilter f; f.name = name; f.folder = QObject::tr("Essentials"); f.command = command; f.params = std::move(params); list.push_back(f);
    };
    add("Sharpen (Unsharp Mask)", "unsharp", {number("Radius", 2, 0.5, 20, 1), number("Amount", 1.5, 0, 5, 2), number("Threshold", 0, 0, 50, 0)});
    add("Sharpen (Richardson-Lucy)", "deblur_richardsonlucy", {number("Sigma", 2, 0.5, 5, 1), number("Iterations", 10, 1, 50, 0)});
    add("Sharpen (Simple)", "sharpen", {number("Amplitude", 200, 0, 1000, 0)});
    add("Smooth (Anisotropic)", "smooth", {number("Amplitude", 40, 0, 200, 0), number("Sharpness", 0.7, 0, 2, 2), number("Anisotropy", 0.3, 0, 1, 2), number("Alpha", 0.6, 0, 2, 2), number("Sigma", 1.1, 0, 4, 2)});
    add("Smooth (Bilateral)", "bilateral", {number("Spatial", 10, 1, 50, 0), number("Value", 10, 1, 100, 0)});
    add("Smooth (Kuwahara)", "kuwahara", {number("Size", 5, 1, 20, 0)});
    add("Denoise (Patch-based)", "denoise", {number("Spatial", 10, 1, 50, 0), number("Value", 10, 1, 50, 0), number("Patch size", 8, 3, 15, 0), number("Lookup size", 4, 3, 15, 0)});
    add("Cartoon", "cartoon", {number("Smoothness", 3, 0, 10, 1), number("Sharpening", 150, 0, 400, 0), number("Threshold", 20, 0, 100, 0), number("Thickness", 0.25, 0, 2, 2), number("Color", 1.5, 0, 3, 1), number("Quantization", 8, 2, 64, 0)});
    add("Vignette", "vignette", {number("Strength", 70, 0, 100, 0), number("Inner radius", 70, 0, 100, 0), number("Outer radius", 95, 0, 100, 0)});
    add("Old Photo", "old_photo");
    add("Sepia", "sepia");
    add("Equalize Histogram", "equalize", {number("Levels", 256, 2, 256, 0)});
    add("Normalize", "normalize 0,255");
    return list;
}

} // namespace

GmicDialog::GmicDialog(EditorSession* session, QWidget* parent) : QDialog(parent), session_(session), presets_(builtinPresets()) {
    setWindowTitle(tr("G'MIC"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 600);
    auto* layout = new QVBoxLayout(this);
    auto* splitter = new QSplitter;

    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Search filters"));
    search_->setClearButtonEnabled(true);
    leftLayout->addWidget(search_);
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    leftLayout->addWidget(tree_, 1);
    catalogueInfo_ = new QLabel;
    catalogueInfo_->setWordWrap(true);
    catalogueInfo_->setStyleSheet(hintStyle());
    leftLayout->addWidget(catalogueInfo_);
    update_ = new QPushButton(tr("Update Filters…"));
    update_->setToolTip(tr("Download the filter definitions for the installed G'MIC version from gmic.eu"));
    leftLayout->addWidget(update_);
    splitter->addWidget(left);

    auto* right = new QWidget;
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    controls_ = new QWidget;
    controlsLayout_ = new QVBoxLayout(controls_);
    controlsLayout_->setAlignment(Qt::AlignTop);
    scroll->setWidget(controls_);
    rightLayout->addWidget(scroll, 1);
    auto* commandRow = new QHBoxLayout;
    commandRow->addWidget(new QLabel(tr("Command")));
    command_ = new QLineEdit;
    command_->setToolTip(tr("The G'MIC command line that runs on the layer; edit it freely"));
    commandRow->addWidget(command_, 1);
    rightLayout->addLayout(commandRow);
    auto* bottom = new QHBoxLayout;
    preview_ = new QCheckBox(tr("Preview"));
    preview_->setChecked(true);
    bottom->addWidget(preview_);
    status_ = new QLabel;
    status_->setStyleSheet(hintStyle());
    status_->setWordWrap(true);
    bottom->addWidget(status_, 1);
    rightLayout->addLayout(bottom);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    ok_->setText(tr("Apply"));
    connect(buttons, &QDialogButtonBox::accepted, this, &GmicDialog::applyAndClose);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    debounce_.setSingleShot(true);
    debounce_.setInterval(350);
    connect(&debounce_, &QTimer::timeout, this, &GmicDialog::runPreview);
    connect(&preview_runner_, &GmicRunner::finished, this, &GmicDialog::previewFinished);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) { fillTree(text); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (!item || item->data(0, Qt::UserRole).isNull()) return;
        int index = item->data(0, Qt::UserRole).toInt();
        bool builtin = item->data(0, Qt::UserRole + 1).toBool();
        const auto& list = builtin ? presets_ : catalogue_.filters();
        if (index >= 0 && index < int(list.size())) selectFilter(&list[size_t(index)]);
    });
    connect(command_, &QLineEdit::textEdited, this, [this] { customCommand_ = true; schedulePreview(); });
    connect(preview_, &QCheckBox::toggled, this, [this](bool on) { if (on) schedulePreview(); else session_->clearPixelPreview(); });
    connect(update_, &QPushButton::clicked, this, &GmicDialog::updateFilters);

    source_ = session_->adjustmentSource(0, transform_);
    if (source_) {
        previewSource_ = previewCopy(source_, previewLimit, previewScale_);
        coverage_ = session_->selectionOnGrid(transform_, source_->width(), source_->height());
        previewCoverage_ = previewSource_ == source_ ? coverage_ : coverageCopy(coverage_, previewSource_->width(), previewSource_->height());
    }
    if (!GmicRunner::available()) {
        status_->setText(tr("G'MIC is not installed. Install the gmic package (Arch: pacman -S gmic; Ubuntu: apt install gmic) and reopen this dialog."));
        ok_->setEnabled(false);
        update_->setEnabled(false);
    }
    loadCatalogue();
    fillTree({});
    // Start on the first essential so the right side is never blank.
    if (QTreeWidgetItem* first = tree_->topLevelItem(0); first && first->childCount() > 0) tree_->setCurrentItem(first->child(0));
}

GmicDialog::~GmicDialog() {
    preview_runner_.cancel();
    if (!finished_) session_->clearPixelPreview();
}

void GmicDialog::loadCatalogue() {
    QString path = GmicCatalogue::preferredFile();
    QString error;
    if (!path.isEmpty() && catalogue_.load(path, &error)) {
        catalogueInfo_->setText(tr("%1 filters from %2 (G'MIC %3)").arg(catalogue_.filters().size()).arg(QFileInfo(path).fileName(), GmicRunner::version()));
    } else {
        catalogue_.load({});
        catalogueInfo_->setText(GmicRunner::executable().isEmpty() ? QString() : tr("Only the essentials are listed until the full catalogue is downloaded with Update Filters (about 1 MB from gmic.eu)."));
    }
}

void GmicDialog::fillTree(const QString& search) {
    tree_->clear();
    QString needle = search.trimmed();
    auto addList = [&](const std::vector<GmicFilter>& list, bool builtin) {
        std::map<QString, QTreeWidgetItem*> folders;
        for (size_t i = 0; i < list.size(); i++) {
            const GmicFilter& f = list[i];
            if (!needle.isEmpty() && !f.name.contains(needle, Qt::CaseInsensitive) && !f.folder.contains(needle, Qt::CaseInsensitive)) continue;
            QTreeWidgetItem*& folder = folders[f.folder];
            if (!folder) { folder = new QTreeWidgetItem(tree_, {f.folder.isEmpty() ? tr("Filters") : f.folder}); folder->setFlags(Qt::ItemIsEnabled); }
            auto* item = new QTreeWidgetItem(folder, {f.name});
            item->setData(0, Qt::UserRole, int(i));
            item->setData(0, Qt::UserRole + 1, builtin);
        }
        for (auto& [name, item] : folders) item->setExpanded(!needle.isEmpty() || builtin);
    };
    addList(presets_, true);
    addList(catalogue_.filters(), false);
    if (!needle.isEmpty()) tree_->expandAll();
}

void GmicDialog::selectFilter(const GmicFilter* filter) {
    current_ = *filter;
    customCommand_ = false;
    buildControls();
    updateCommand();
}

void GmicDialog::buildControls() {
    while (QLayoutItem* item = controlsLayout_->takeAt(0)) { delete item->widget(); delete item; }
    auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(current_.name.toHtmlEscaped()));
    controlsLayout_->addWidget(title);
    for (size_t i = 0; i < current_.params.size(); i++) {
        GmicParam& p = current_.params[i];
        switch (p.kind) {
        case GmicParam::Float: case GmicParam::Int: {
            auto* row = new QHBoxLayout;
            auto* label = new QLabel(p.label);
            label->setMinimumWidth(120);
            row->addWidget(label);
            auto* slider = new QSlider(Qt::Horizontal);
            const double scale = p.kind == GmicParam::Int ? 1 : std::pow(10.0, p.decimals);
            slider->setRange(int(std::floor(p.min * scale)), int(std::ceil(p.max * scale)));
            slider->setValue(int(std::lround(p.value * scale)));
            row->addWidget(slider, 1);
            auto* spin = new QDoubleSpinBox;
            spin->setRange(p.min, p.max);
            spin->setDecimals(p.decimals);
            spin->setValue(p.value);
            spin->setKeyboardTracking(false);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setFixedWidth(72);
            row->addWidget(spin);
            connect(slider, &QSlider::valueChanged, this, [this, &p, spin, scale](int v) { p.value = v / scale; { QSignalBlocker b(spin); spin->setValue(p.value); } updateCommand(); });
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, &p, slider, scale](double v) { p.value = v; { QSignalBlocker b(slider); slider->setValue(int(std::lround(v * scale))); } updateCommand(); });
            controlsLayout_->addLayout(row);
            break;
        }
        case GmicParam::Bool: {
            auto* box = new QCheckBox(p.label);
            box->setChecked(p.value != 0);
            connect(box, &QCheckBox::toggled, this, [this, &p](bool on) { p.value = on ? 1 : 0; updateCommand(); });
            controlsLayout_->addWidget(box);
            break;
        }
        case GmicParam::Choice: {
            auto* row = new QHBoxLayout;
            auto* label = new QLabel(p.label);
            label->setMinimumWidth(120);
            row->addWidget(label);
            auto* combo = new QComboBox;
            combo->addItems(p.choices);
            combo->setCurrentIndex(int(p.value));
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, &p](int i) { p.value = i; updateCommand(); });
            row->addWidget(combo, 1);
            controlsLayout_->addLayout(row);
            break;
        }
        case GmicParam::Color: {
            auto* row = new QHBoxLayout;
            auto* label = new QLabel(p.label);
            label->setMinimumWidth(120);
            row->addWidget(label);
            auto* button = new QPushButton;
            auto paint = [button, &p] { button->setStyleSheet(QStringLiteral("background: rgb(%1,%2,%3);").arg(int(p.r)).arg(int(p.g)).arg(int(p.b))); button->setText(QStringLiteral("%1, %2, %3").arg(int(p.r)).arg(int(p.g)).arg(int(p.b))); };
            paint();
            connect(button, &QPushButton::clicked, this, [this, &p, paint] {
                QColor c = QColorDialog::getColor(QColor(int(p.r), int(p.g), int(p.b), int(p.a)), this, p.label, p.hasAlpha ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions());
                if (!c.isValid()) return;
                p.r = c.red(); p.g = c.green(); p.b = c.blue(); if (p.hasAlpha) p.a = c.alpha();
                paint();
                updateCommand();
            });
            row->addWidget(button, 1);
            controlsLayout_->addLayout(row);
            break;
        }
        case GmicParam::Text: {
            auto* row = new QHBoxLayout;
            auto* label = new QLabel(p.label);
            label->setMinimumWidth(120);
            row->addWidget(label);
            auto* edit = new QLineEdit(p.text);
            connect(edit, &QLineEdit::textChanged, this, [this, &p](const QString& t) { p.text = t; updateCommand(); });
            row->addWidget(edit, 1);
            controlsLayout_->addLayout(row);
            break;
        }
        case GmicParam::Note: {
            auto* note = new QLabel(p.text);
            note->setWordWrap(true);
            note->setStyleSheet(hintStyle());
            note->setOpenExternalLinks(true);
            controlsLayout_->addWidget(note);
            break;
        }
        case GmicParam::Separator: {
            auto* line = new QFrame;
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            controlsLayout_->addWidget(line);
            break;
        }
        default: break;
        }
    }
}

void GmicDialog::updateCommand() {
    if (!customCommand_) { QSignalBlocker b(command_); command_->setText(current_.commandLine(false)); }
    schedulePreview();
}

void GmicDialog::schedulePreview() {
    if (!preview_->isChecked() || !previewSource_ || GmicRunner::executable().isEmpty()) return;
    debounce_.start();
}

void GmicDialog::runPreview() {
    if (applying_ || !previewSource_) return;
    QString command = customCommand_ ? command_->text().trimmed() : current_.commandLine(true);
    if (command.isEmpty()) { session_->clearPixelPreview(); return; }
    status_->setText(tr("Previewing…"));
    preview_runner_.start(previewSource_, command);
}

void GmicDialog::previewFinished(std::shared_ptr<Image> result, QString error) {
    if (applying_) return;
    if (!result) { status_->setText(error); session_->clearPixelPreview(); return; }
    status_->clear();
    if (previewCoverage_) blendThroughCoverage(*result, *previewSource_, *previewCoverage_);
    session_->setPixelPreview(result, std::nullopt);
    if (debounce_.isActive()) return;   // a newer preview is already scheduled
}

void GmicDialog::applyAndClose() {
    if (!source_ || applying_) return;
    QString command = customCommand_ ? command_->text().trimmed() : current_.commandLine(false);
    if (command.isEmpty()) { reject(); return; }
    applying_ = true;
    preview_runner_.cancel();
    setEnabled(false);
    status_->setText(tr("Applying %1…").arg(current_.name));
    auto* runner = new GmicRunner(this);
    connect(runner, &GmicRunner::finished, this, [this, runner, command](std::shared_ptr<Image> result, QString error) {
        runner->deleteLater();
        setEnabled(true);
        applying_ = false;
        if (!result) { status_->setText(error); QMessageBox::warning(this, tr("G'MIC"), error); return; }
        if (coverage_) blendThroughCoverage(*result, *source_, *coverage_);
        finished_ = true;
        session_->commitPixels(result, transform_, tr("G'MIC: %1").arg(customCommand_ ? command.section(' ', 0, 0) : current_.name));
        QDialog::done(QDialog::Accepted);
    });
    runner->start(source_, command);
}

void GmicDialog::done(int result) {
    if (finished_) { QDialog::done(result); return; }
    if (result == QDialog::Accepted) { applyAndClose(); return; }
    finished_ = true;
    preview_runner_.cancel();
    session_->clearPixelPreview();
    QDialog::done(result);
}

void GmicDialog::updateFilters() {
    if (!network_) network_ = new QNetworkAccessManager(this);
    QUrl url(GmicCatalogue::updateUrl());
    update_->setEnabled(false);
    status_->setText(tr("Downloading %1…").arg(url.toString()));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        update_->setEnabled(true);
        if (reply->error() != QNetworkReply::NoError) { status_->setText(tr("Download failed: %1").arg(reply->errorString())); return; }
        QByteArray data = reply->readAll();
        QString path = GmicCatalogue::ownFile();
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(data) != data.size()) { status_->setText(tr("Couldn't save %1").arg(path)); return; }
        file.close();
        loadCatalogue();
        fillTree(search_->text());
        status_->setText(tr("%1 filters ready.").arg(catalogue_.filters().size()));
    });
}

} // namespace app
