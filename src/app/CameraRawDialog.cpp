// Filter > Camera Raw Filter: the Camera Raw panels (CameraRawPanels.h, a port of upstream Compositor's panels, MIT,
// see LICENSES/MIT-Compositor.txt) over the pinned layer, previewed on the canvas; the grade is compositor/cameraraw.h.
#include "CameraRawDialog.h"
#include "CameraRawPanels.h"
#include <QCheckBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVBoxLayout>
#include <random>

using namespace compositor;

namespace app {

namespace {

/// Big enough for the radii to read as they will, small enough to follow a slider on a large photo.
constexpr int previewLimit = 1600;

/// The grade the last OK left, offered again on the next open as upstream does.
CameraRawSettings& remembered() {
    static CameraRawSettings settings;
    return settings;
}

} // namespace

CameraRawDialog::CameraRawDialog(EditorSession* session, QWidget* parent)
    : PixelDialog(session, parent), seed_(uint32_t(std::random_device{}() & 0x7fffffffu)) {   // within pixels.cameraRaw's integer seed
    setWindowTitle(tr("Camera Raw Filter"));
    setMinimumWidth(440);
    resize(540, 640);
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(15);
    connect(debounce_, &QTimer::timeout, this, [this] { refreshPreview(); });

    auto* layout = new QVBoxLayout(this);
    panels_ = new CameraRawPanels(CameraRawPanels::Mode::Filter, remembered(), this);
    // White Balance > Auto: the gray-world balance of the layer's covered pixels.
    panels_->autoBalance = [this]() -> std::optional<std::array<double, 2>> {
        if (!hasSource()) return std::nullopt;
        return source16() ? CameraRawSettings::autoBalance(*source16()) : CameraRawSettings::autoBalance(*source());
    };
    connect(panels_, &CameraRawPanels::changed, this, [this] { debounce_->start(); });
    layout->addWidget(panels_, 1);
    connect(addPreviewAndButtons(layout), &QCheckBox::toggled, this, [this] { refreshPreview(); });

    capture(0, previewLimit);
    refreshPreview();
}

// ---- preview and commit -----------------------------------------------------------------------------

std::shared_ptr<Image> CameraRawDialog::run(const Image& source, double scale, const CameraRawPreview& preview) const {
    auto out = std::make_shared<Image>(source);
    applyCameraRaw(*out, panels_->settings(), scale, seed_, preview);
    return out;
}

std::shared_ptr<Image16> CameraRawDialog::run(const Image16& source, double scale, const CameraRawPreview& preview) const {
    auto out = std::make_shared<Image16>(source);
    applyCameraRaw(*out, panels_->settings(), scale, seed_, preview);
    return out;
}

void CameraRawDialog::refreshPreview() {
    if (finished() || !hasPreviewSource()) return;
    const bool overlays = panels_->preview().shadowClipIndicator || panels_->preview().highlightClipIndicator || panels_->preview().sharpenMask;
    if (!previewing() || (panels_->settings().normalized().isIdentity() && !overlays)) { clearPreview(); return; }
    if (previewSource16()) showPreview(run(*previewSource16(), previewScale(), panels_->preview()), placement());
    else showPreview(run(*previewSource(), previewScale(), panels_->preview()), placement());
}

bool CameraRawDialog::apply() {
    remembered() = panels_->settings();
    if (panels_->settings().normalized().isIdentity()) return true;   // an unchanged grade is not an edit
    // The command path: pixels.cameraRaw with the grade and this dialog's grain seed.
    const QJsonObject settings = QJsonDocument::fromJson(QByteArray::fromStdString(panels_->settings().toJson())).object();
    if (commitAsCommand(QStringLiteral("pixels.cameraRaw"), {{"settings", settings}, {"seed", qint64(seed_)}})) return true;
    if (source16()) {
        auto out = run(*source16(), 1, {});
        throughSelection(*out);
        commit(Image16Ptr(out), placement(), QT_TRANSLATE_NOOP("History", "Camera Raw Filter"));
        return true;
    }
    auto out = run(*source(), 1, {});
    throughSelection(*out);
    commit(out, placement(), QT_TRANSLATE_NOOP("History", "Camera Raw Filter"));
    return true;
}

} // namespace app
