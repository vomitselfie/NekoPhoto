// The Filter menu: the filters by group, G'MIC and Mosh (MainWindow::buildMenus, MainWindowMenus.h).
#include "MainWindowMenus.h"
#include "Names.h"
#include "CanvasFrame.h"
#include "CameraRawDialog.h"
#include "FilterDialog.h"
#include "GridFilters.h"
#include "nekophoto/smartfilter.h"
#include <random>
#include "MoshDialog.h"
#include "PreferencesDialog.h"
#include <QMenuBar>
#include <QMessageBox>

using namespace nekophoto;

namespace app {

using namespace menus;

void MainWindow::buildFilterMenu(MenuContext& m) {
    QMenu* filter = menuBar()->addMenu(tr("Filte&r"));
    // Photoshop's filters on a smart object go on as Smart Filters: the blurs, noise and the ones the Smart Filter
    // kernels draw. The rest change pixels, so on a smart object they ask to rasterize first.
    auto smartCapable = [](FilterKind kind) {
        return kind == FilterKind::GaussianBlur || kind == FilterKind::MotionBlur || kind == FilterKind::AddNoise
               || nekophoto::smartFilterParametersFor(kind, nekophoto::FilterSettings::defaults(kind)).has_value();
    };
    auto filterAction = [this, &m, smartCapable](QMenu* menu, const QString& group, const QString& label, FilterKind kind) {
        m.add(menu, Spec(QStringLiteral("filter.") + group + commandSlug(QString::fromUtf8(filterKindName(kind))), label)
                      .document((std::string("filter.") + filterKindName(kind)).c_str()).runs([this, kind, smartCapable] {
            if (smartCapable(kind) && session_->canAddSmartFilter()) { (new FilterDialog(session_, kind, this, true))->show(); return; }
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
            if (nekophoto::filterRunsDirectly(kind)) {
                // No dialog, as in Photoshop: Clouds draws a new pattern each time.
                const uint32_t seed = uint32_t(std::random_device{}() % 1000000000u);
                runCommand("pixels.filter", gridFilterStep(kind, nekophoto::FilterSettings::defaults(kind), seed), names::filterKind(kind));
                return;
            }
            (new FilterDialog(session_, kind, this))->show();
        }, "pixels.filter"));
    };
    // Photoshop's places: the top-level filters, then the submenus in its order, each in its own order.
    // Photoshop's shortcut. A destructive filter here: on a smart object it asks first, like the others. OK runs
    // pixels.cameraRaw.
    m.add(filter, Spec("filter.cameraRaw", tr("Camera &Raw Filter…"), {QKeySequence("Shift+Ctrl+A")}).document("filter.Camera Raw").runs([this] {
        if (session_->smartObjectBlocksPixels(true)) return;
        if (!session_->canAdjustPixels()) { showError(tr("Camera Raw Filter"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
        (new CameraRawDialog(session_, this))->show();
    }, "pixels.cameraRaw"));
    filterAction(filter, QString(), tr("&Lens Correction…"), FilterKind::LensCorrection);
    filter->addSeparator();
    QMenu* blurMenu = filter->addMenu(tr("&Blur"));
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Box Blur…"), FilterKind::BoxBlur);
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Gaussian Blur…"), FilterKind::GaussianBlur);
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Motion Blur…"), FilterKind::MotionBlur);
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Radial Blur…"), FilterKind::RadialBlur);
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Surface Blur…"), FilterKind::SurfaceBlur);
    QMenu* distortMenu = filter->addMenu(tr("&Distort"));
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Pinch…"), FilterKind::Pinch);
    filterAction(distortMenu, QStringLiteral("distort."), tr("P&olar Coordinates…"), FilterKind::PolarCoordinates);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Ripple…"), FilterKind::Ripple);
    filterAction(distortMenu, QStringLiteral("distort."), tr("S&hear…"), FilterKind::Shear);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Spherize…"), FilterKind::Spherize);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Twirl…"), FilterKind::Twirl);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Wave…"), FilterKind::Wave);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&ZigZag…"), FilterKind::ZigZag);
    QMenu* noiseMenu = filter->addMenu(tr("&Noise"));
    filterAction(noiseMenu, QStringLiteral("noise."), tr("Add &Noise…"), FilterKind::AddNoise);
    filterAction(noiseMenu, QStringLiteral("noise."), tr("&Dust && Scratches…"), FilterKind::DustAndScratches);
    filterAction(noiseMenu, QStringLiteral("noise."), tr("&Median…"), FilterKind::Median);
    QMenu* pixelateMenu = filter->addMenu(tr("&Pixelate"));
    filterAction(pixelateMenu, QStringLiteral("pixelate."), tr("&Mosaic…"), FilterKind::Mosaic);
    QMenu* renderMenu = filter->addMenu(tr("R&ender"));
    filterAction(renderMenu, QStringLiteral("render."), tr("&Clouds"), FilterKind::Clouds);
    filterAction(renderMenu, QStringLiteral("render."), tr("&Difference Clouds"), FilterKind::DifferenceClouds);
    QMenu* sharpenMenu = filter->addMenu(tr("S&harpen"));
    filterAction(sharpenMenu, QStringLiteral("sharpen."), tr("&Unsharp Mask…"), FilterKind::UnsharpMask);
    QMenu* stylizeMenu = filter->addMenu(tr("St&ylize"));
    filterAction(stylizeMenu, QStringLiteral("stylize."), tr("&Emboss…"), FilterKind::Emboss);
    filterAction(stylizeMenu, QStringLiteral("stylize."), tr("&Find Edges"), FilterKind::FindEdges);
    QMenu* otherMenu = filter->addMenu(tr("O&ther"));
    filterAction(otherMenu, QStringLiteral("other."), tr("&High Pass…"), FilterKind::HighPass);
    filterAction(otherMenu, QStringLiteral("other."), tr("Ma&ximum…"), FilterKind::Maximum);
    filterAction(otherMenu, QStringLiteral("other."), tr("M&inimum…"), FilterKind::Minimum);
    filterAction(otherMenu, QStringLiteral("other."), tr("&Offset…"), FilterKind::Offset);
    filter->addSeparator();
    // OpenMosh's glitch, distortion and retro effects (docs/mosh.md), a submenu per category; OK runs pixels.mosh.
    QMenu* moshMenu = filter->addMenu(tr("M&osh"));
    for (int c = 0; c < nekophoto::mosh::categoryCount; c++) {
        const auto category = nekophoto::mosh::Category(c);
        QMenu* sub = nullptr;
        for (const nekophoto::mosh::EffectSpec& spec : nekophoto::mosh::effects()) {
            if (spec.category != category) continue;
            if (!sub) sub = moshMenu->addMenu(names::mosh(nekophoto::mosh::categoryName(category)));
            const nekophoto::mosh::EffectSpec* effect = &spec;
            m.add(sub, Spec(QStringLiteral("filter.mosh.") + commandSlug(QString::fromUtf8(spec.id.data(), qsizetype(spec.id.size()))), tr("%1…").arg(names::mosh(spec.name)))
                         .document("filter.Mosh").runs([this, effect] {
                if (session_->smartObjectBlocksPixels(true)) return;
                if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
                (new MoshDialog(session_, *effect, this))->show();
            }, "pixels.mosh"));
        }
    }
    filter->addSeparator();
    gmicAction_ = m.add(filter, Spec("filter.gmic", tr("&G'MIC…"), {QKeySequence("Ctrl+Shift+G")}).document("filter.G'MIC").runs([this] { openGmic(); }, "pixels.gmic"));
    removeBackgroundAction_ = m.add(filter, Spec("filter.removeBackground", tr("Remove &Background…")).document("edit.removeBackground").runs([this] {
        if (!ModelStore::ready()) {
            // Off, or no model yet: the preferences page is where it gets turned on and fetched.
            auto answer = QMessageBox::question(this, tr("Remove Background"),
                ModelStore::supported() ? tr("AI background removal is turned off. Open Preferences to enable it and download the model?")
                                        : tr("This build was made without OpenCV, which runs the segmentation model."),
                ModelStore::supported() ? (QMessageBox::Yes | QMessageBox::Cancel) : QMessageBox::Ok);
            if (answer == QMessageBox::Yes) showPreferences();
            return;
        }
        if (session_->smartObjectBlocksPixels(true)) return;
        if (!session_->canAdjustPixels()) { showError(tr("Remove Background"), tr("Select a visible image layer (not a mask) to remove its background.")); return; }
        const ModelInfo* quick = ModelStore::modelById("pphumanseg");
        QString quickPath = quick && ModelStore::isPresent(*quick) ? ModelStore::pathFor(*quick) : QString();
        (new BackgroundDialog(session_, ModelStore::pathFor(ModelStore::selected()), quickPath, this))->show();
    }, "pixels.removeBackground"));
    refreshBackgroundAction();
}

} // namespace app
