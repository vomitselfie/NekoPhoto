#include "compositor/seamcarve.h"
#include "EditorSession.h"
#include "compositor/depth.h"
#include "ColorManagement.h"
#include "ImageConvert.h"
#include "QtGeometry.h"
#include "compositor/filters.h"
#include "compositor/inpaint.h"
#include "compositor/contentmove.h"
#include "compositor/seamcarve.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <type_traits>

using namespace compositor;

namespace app {

// ---- Moving selected pixels ------------------------------------------------------------------

bool EditorSession::canMovePixels(QPointF documentPoint) const {
    if (!document_ || !document_->selection || !document_->selection->coverage || pixelMove_ || stroke_ || transformEdit_ || isMaskSelected_) return false;
    const Layer* layer = activeLayer();
    if (!layer || !layer->asset || layer->isGroup || layer->adjustment) return false;
    int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= document_->width || y >= document_->height) return false;
    auto coverage = selectionCoverage8();
    return coverage && coverage->at(x, y) > 127;
}

bool EditorSession::beginPixelMove(bool duplicate) {
    if (refusedAtDepth("edit.movePixels", tr("Editing pixels"))) return false;
    if (pixelMove_ || !document_ || !document_->selection || !document_->selection->coverage || document_->selection->isEmpty() || isMaskSelected_) return false;
    const Layer* layer = activeLayer();
    if (!layer || !layer->asset || layer->isGroup || layer->adjustment || stroke_ || transformEdit_) return false;
    resolveGradient();
    auto raster = makeRasterEdit(*layer, false, BrushSettings());
    if (!raster || !raster->liftSelection()) return false;
    endOpacityEdit();
    pixelMove_ = std::make_unique<PixelMove>();
    pixelMove_->raster = std::move(raster);
    pixelMove_->origin = *document_->selection;
    pixelMove_->duplicate = duplicate;
    pixelMove_->layerId = layer->id;
    return true;
}

void EditorSession::movePixels(QPointF offset) {
    if (refusedAtDepth("edit.movePixels", tr("Editing pixels"))) return;
    if (!pixelMove_) return;
    QPointF rounded(std::round(offset.x()), std::round(offset.y()));
    pixelMove_->raster->moveLifted(toPoint(rounded), pixelMove_->duplicate);
    pixelMove_->offset = rounded;
    emit documentChanged({});
    emit selectionChanged();
}

std::optional<Selection> EditorSession::displayedSelection() const {
    if (!document_ || !document_->selection) return std::nullopt;
    if (pixelMove_ && pixelMove_->origin.coverage) {
        int dx = int(pixelMove_->offset.x()), dy = int(pixelMove_->offset.y());
        if (dx == 0 && dy == 0) return pixelMove_->origin;
        return offsetSelection(pixelMove_->origin, dx, dy);
    }
    if (transformEdit_ && transformEdit_->floating && document_->selection->coverage.f32()) {
        Selection s = *document_->selection;
        if (auto moved = floatingSelectionF(*transformEdit_)) s.coverage = GrayFPtr(moved);
        return s;
    }
    if (transformEdit_ && transformEdit_->floating && document_->selection->coverage.u16()) {
        const FloatingTransform& f = *transformEdit_->floating;
        const Gray16& cov = *document_->selection->coverage.u16();
        std::shared_ptr<Gray16> moved;
        if (transformEdit_->corners) moved = warpCoverage(cov, f.original, f.pixelWidth, f.pixelHeight, *transformEdit_->corners);
        else {
            moved = std::make_shared<Gray16>(cov.width(), cov.height(), 0);
            const Affine map = f.original.pixelToDocument(f.pixelWidth, f.pixelHeight).inverted().concatenating(transformEdit_->draft.pixelToDocument(f.pixelWidth, f.pixelHeight));
            const Affine inv = map.inverted();
            for (int y = 0; y < cov.height(); y++) for (int x = 0; x < cov.width(); x++) {
                const Point p = inv.apply({x + 0.5, y + 0.5});
                const int sx = int(std::floor(p.x)), sy = int(std::floor(p.y));
                if (sx >= 0 && sy >= 0 && sx < cov.width() && sy < cov.height()) moved->at(x, y) = cov.at(sx, sy);
            }
        }
        Selection s = *document_->selection;
        s.coverage = Gray16Ptr(moved);
        return s;
    }
    if (transformEdit_ && transformEdit_->floating && document_->selection->coverage) {
        const FloatingTransform& f = *transformEdit_->floating;
        const GrayImage& cov = *document_->selection->coverage.u8();
        auto moved = std::make_shared<GrayImage>(cov.width(), cov.height(), 0);
        if (transformEdit_->corners) moved = warpCoverage(cov, f.original, f.pixelWidth, f.pixelHeight, *transformEdit_->corners);
        else {
            Affine map = f.original.pixelToDocument(f.pixelWidth, f.pixelHeight).inverted().concatenating(transformEdit_->draft.pixelToDocument(f.pixelWidth, f.pixelHeight));
            Affine inv = map.inverted();
            for (int y = 0; y < cov.height(); y++) for (int x = 0; x < cov.width(); x++) {
                Point p = inv.apply({x + 0.5, y + 0.5});
                int sx = int(std::floor(p.x)), sy = int(std::floor(p.y));
                if (sx >= 0 && sy >= 0 && sx < cov.width() && sy < cov.height()) moved->at(x, y) = cov.at(sx, sy);
            }
        }
        Selection s = *document_->selection;
        s.coverage = moved;
        return s;
    }
    return document_->selection;
}

void EditorSession::finishPixelMove() {
    if (!pixelMove_) return;
    std::unique_ptr<PixelMove> move = std::move(pixelMove_);
    if (move->offset.isNull()) { emit documentChanged({}); emit selectionChanged(); return; }
    Selection moved = *[&] { pixelMove_ = std::move(move); auto s = displayedSelection(); move = std::move(pixelMove_); return s; }();
    Layer* layer = document_->find(move->layerId);
    if (!layer) return;
    BrushStroke::Commit commit = move->raster->commit();
    beginEdit(move->duplicate ? QT_TRANSLATE_NOOP("History", "Duplicate Pixels") : QT_TRANSLATE_NOOP("History", "Move Pixels"));
    if (commit.asset) {
        if (layer->mask && layer->mask->linked && !layer->mask->placement && !commit.transform.samePlacement(layer->transform)) layer->mask->placement = layer->transform;
        layer->asset = commit.asset;
        if (document_->colorMode != ColorMode::RGB) layer->asset->thumbnail = modeThumbnail(layer->asset->image, document_->colorMode, document_->profile);
        layer->transform = commit.transform;
        layer->shapeImage.reset();
    }
    document_->selection = moved;
    endEdit();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::cancelPixelMove() {
    if (!pixelMove_) return;
    pixelMove_.reset();
    emit documentChanged({});
    emit selectionChanged();
}

void EditorSession::nudgePixels(double dx, double dy) {
    if (refusedAtDepth("edit.movePixels", tr("Editing pixels"))) return;
    if (!beginPixelMove(false)) return;
    movePixels({dx, dy});
    finishPixelMove();
}

std::optional<QColor> EditorSession::compositeColorAt(QPointF documentPoint) const {
    if (document_ && (document_->sampleType == SampleType::F32 || document_->colorMode != ColorMode::RGB)) {
        const std::optional<NativeSample> sample = nativeColorAt(documentPoint);
        if (!sample) return std::nullopt;
        return sample->color;
    }
    if (document_ && document_->sampleType == SampleType::U16) {
        // The 16-bit composite, not the dithered one, so a flat area samples one colour.
        auto deep = flattened16();
        if (!deep) return std::nullopt;
        int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
        if (x < 0 || y < 0 || x >= deep->width() || y >= deep->height()) return std::nullopt;
        const uint16_t* p = deep->pixel(x, y);
        if (!p[3]) return std::nullopt;
        auto channel = [&](int c) { return int((uint64_t(p[c]) * 255 + p[3] / 2) / p[3]); };
        return QColor(std::min(channel(0), 255), std::min(channel(1), 255), std::min(channel(2), 255));
    }
    auto flat = flattened();
    if (!flat) return std::nullopt;
    int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= flat->width() || y >= flat->height()) return std::nullopt;
    const uint8_t* p = flat->pixel(x, y);
    if (!p[3]) return std::nullopt;
    return QColor(p[0] * 255 / p[3], p[1] * 255 / p[3], p[2] * 255 / p[3]);
}

std::optional<EditorSession::NativeSample> EditorSession::nativeColorAt(QPointF documentPoint) const {
    if (!document_) return std::nullopt;
    const int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= document_->width || y >= document_->height) return std::nullopt;
    RenderOptions options;
    options.region = Rect(x, y, 1, 1);
    NativeSample sample;
    const Document& doc = *document_;
    if (doc.sampleType == SampleType::F32) {
        // The composite's linear value, as the pixel holds it; the colour pickers take it encoded through the document's
        // curve (light above white shows as white there; `values` keep it).
        ImageF out;
        renderF(doc, options, out);
        if (out.width() < 1 || out.height() < 1) return std::nullopt;
        const float* p = out.pixel(0, 0);
        if (!(p[3] > 0)) return std::nullopt;
        const TransferCurve curve = documentCurve();
        float encoded[3];
        for (int c = 0; c < 3; c++) {
            sample.values.push_back(p[c] / p[3]);
            encoded[c] = curve.fromLinearExact(std::clamp(p[c] / p[3], 0.0f, 1.0f));
        }
        sample.color = QColor::fromRgbF(encoded[0], encoded[1], encoded[2]);
        return sample;
    }
    // CMYK and Lab: the stored samples made straight, read as inks (0..100) or L, a, b, then through the document's
    // profile to the sRGB the colour pickers hold in these modes.
    const AnyImage native = renderNative(doc, options);
    if (!native || native.width() < 1) return std::nullopt;
    const int n = colorModeChannels(doc.colorMode);
    const bool deep = doc.sampleType == SampleType::U16;
    const double one = deep ? double(one16) : 255.0;
    double straight[5] = {0, 0, 0, 0, 0};
    double alpha = 0;
    {
        double raw[5] = {0, 0, 0, 0, 0};
        for (int c = 0; c < n; c++) raw[c] = native.u16() ? native.u16()->pixel(0, 0)[c] : native.c8() ? native.c8()->pixel(0, 0)[c] : native.u8()->pixel(0, 0)[c];
        alpha = raw[n - 1] / one;
        if (!(alpha > 0)) return std::nullopt;
        for (int c = 0; c < n - 1; c++) straight[c] = std::clamp(raw[c] / alpha / one, 0.0, 1.0);
    }
    float in[4] = {0, 0, 0, 0};
    if (doc.colorMode == ColorMode::CMYK) {
        for (int c = 0; c < 4; c++) { in[c] = float((1 - straight[c]) * 100); sample.values.push_back(in[c]); }
    } else {
        const double offset = deep ? labOffset<SampleType::U16>() : labOffset<SampleType::U8>();
        const double scale = deep ? labScale<SampleType::U16>() : labScale<SampleType::U8>();
        in[0] = float(straight[0] * 100);
        in[1] = float((straight[1] * one - offset) / scale);
        in[2] = float((straight[2] * one - offset) / scale);
        for (int c = 0; c < 3; c++) sample.values.push_back(in[c]);
    }
    const ColorTransformPtr t = transformBetween(doc.profile, ColorProfile(), color::conversionOptions(),
                                                 doc.colorMode == ColorMode::CMYK ? PixelFormat::CMYKFloat : PixelFormat::LabFloat, PixelFormat::RGBFloat);
    if (!t) return std::nullopt;
    float rgb[3] = {0, 0, 0};
    t->apply(in, rgb, 1);
    sample.color = QColor::fromRgbF(std::clamp(rgb[0], 0.0f, 1.0f), std::clamp(rgb[1], 0.0f, 1.0f), std::clamp(rgb[2], 0.0f, 1.0f));
    return sample;
}

// ---- Clipboard and Content-Aware Fill ------------------------------------------------

bool EditorSession::canCopyPixels() const {
    if (!canEditLayers()) return false;
    const Layer* layer = activeLayer();
    if (!layer || (layer->isGroup && !isMaskSelected_)) return false;
    if (document_->selection && document_->selection->isEmpty()) return false;
    return isMaskSelected_ ? layer->mask.has_value() : layer->asset.has_value();
}

std::optional<EditorSession::PixelClipboard> EditorSession::renderSelectedPixels(bool merged) const {
    if (!document_) return std::nullopt;
    Rect region = document_->rect();
    if (document_->selection) {
        if (!document_->selection->coverage || document_->selection->isEmpty()) return std::nullopt;
        region = document_->selection->bounds().intersection(document_->rect());
    }
    if (region.isEmpty()) return std::nullopt;
    RenderOptions options;
    options.region = region;
    const Layer* layer = activeLayer();
    if (!merged && !layer) return std::nullopt;
    if (document_->sampleType == SampleType::F32) return renderSelectedPixelsF(merged, region);
    if (document_->colorMode != ColorMode::RGB) return renderSelectedPixelsNative(merged, region);   // EditorSessionModes.cpp
    const bool deep = document_->sampleType == SampleType::U16;
    // The pixels at the document's depth, then multiplied by the selection's coverage.
    auto take = [&](auto& out, const auto* coverage) -> bool {
        using Out = std::remove_cvref_t<decltype(out)>;
        using Sample = std::remove_cvref_t<decltype(*out.data())>;
        constexpr uint32_t full = std::is_same_v<Sample, uint8_t> ? 255u : one16;
        if (merged) {
            if constexpr (std::is_same_v<Out, Image>) render(*document_, options, out); else render16(*document_, options, out);
        } else if (isMaskSelected_ && layer->mask) {
            // The mask as opaque gray, placed as it sits on the document.
            using Gray = std::conditional_t<std::is_same_v<Out, Image>, GrayImage, Gray16>;
            const uint8_t background8 = layer->mask->placement ? LayerMask::background(*layer->mask->asset.thumbnail) : 0;
            const Sample background = std::is_same_v<Out, Image> ? Sample(background8) : Sample(widen8(background8));
            Gray gray(out.width(), out.height(), background);
            if constexpr (std::is_same_v<Out, Image>) {
                if (!layer->mask->asset.image.u8()) return false;
                sampleMaskCoverage(*layer->mask->asset.image.u8(), layer->maskTransform(), region, 1, gray.at(0, 0), gray, false);
            } else {
                if (!layer->mask->asset.image.u16()) return false;
                sampleMaskCoverage(*layer->mask->asset.image.u16(), layer->maskTransform(), region, 1, gray.at(0, 0), gray, false);
            }
            for (int y = 0; y < out.height(); y++) for (int x = 0; x < out.width(); x++) { Sample* p = out.pixel(x, y); p[0] = p[1] = p[2] = gray.at(x, y); p[3] = Sample(full); }
        } else if (layer->asset && layer->asset->image) {
            Document single(document_->width, document_->height);
            single.sampleType = document_->sampleType;
            Layer copy = *layer;
            copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.maskSourceId.reset();
            single.layers = {copy};
            if constexpr (std::is_same_v<Out, Image>) render(single, options, out); else render16(single, options, out);
        } else return false;
        if (coverage)
            for (int y = 0; y < out.height(); y++) for (int x = 0; x < out.width(); x++) {
                const uint32_t k = std::min<uint32_t>(coverage->at(x + int(region.x), y + int(region.y)), full);
                Sample* p = out.pixel(x, y);
                for (int c = 0; c < 4; c++) p[c] = Sample((p[c] * k + full / 2) / full);
            }
        return true;
    };
    if (deep) {
        auto out = std::make_shared<Image16>(int(region.width), int(region.height));
        const Gray16* coverage = document_->selection ? document_->selection->coverage.u16().get() : nullptr;
        if (document_->selection && !coverage) return std::nullopt;
        if (!take(*out, coverage)) return std::nullopt;
        return PixelClipboard{Image16Ptr(out), QPointF(region.x, region.y)};
    }
    Image out(int(region.width), int(region.height));
    const GrayImage* coverage = document_->selection ? document_->selection->coverage.u8().get() : nullptr;
    if (document_->selection && !coverage) return std::nullopt;
    if (!take(out, coverage)) return std::nullopt;
    return PixelClipboard{std::make_shared<Image>(std::move(out)), QPointF(region.x, region.y)};
}

namespace {

/// What the system clipboard gets: 8 bits whatever the document's depth (32 bits tone-mapped at exposure 0 through
/// the document's curve).
QImage clipboardImage(const AnyImage& image, const TransferCurve& curve) {
    if (image.f32()) return toQImage(*encodeImage8(*image.f32(), curve)).convertToFormat(QImage::Format_ARGB32);
    if (image.u16()) return toQImage(*narrowImage(*image.u16())).convertToFormat(QImage::Format_ARGB32);
    return image.u8() ? toQImage(*image.u8()).convertToFormat(QImage::Format_ARGB32) : QImage();
}

} // namespace

bool EditorSession::copySelection() {
    if (refusedAtDepth("edit.clipboard", tr("Editing pixels"))) return false;
    if (!canCopyPixels()) return false;
    auto copied = renderSelectedPixels(false);
    if (!copied) return false;
    copied->mode = document_->colorMode;
    copied->profile = document_->profile;
    pixelClipboard_ = copied;
    QApplication::clipboard()->setImage(document_->colorMode != ColorMode::RGB ? clipboardImageFor(copied->image) : clipboardImage(copied->image, documentCurve()));
    return true;
}

bool EditorSession::copyMerged() {
    if (refusedAtDepth("edit.clipboard", tr("Editing pixels"))) return false;
    if (!canEditLayers() || (document_->selection && document_->selection->isEmpty())) return false;
    auto copied = renderSelectedPixels(true);
    if (!copied) return false;
    copied->mode = document_->colorMode;
    copied->profile = document_->profile;
    pixelClipboard_ = copied;
    QApplication::clipboard()->setImage(document_->colorMode != ColorMode::RGB ? clipboardImageFor(copied->image) : clipboardImage(copied->image, documentCurve()));
    return true;
}

bool EditorSession::cutSelection() {
    if (refusedAtDepth("edit.clipboard", tr("Editing pixels"))) return false;
    if (!document_ || !document_->selection || !canCopyPixels()) return false;
    if (!copySelection()) return false;
    clearSelectionPixels();
    return true;
}

bool EditorSession::canPaste() const {
    if (!document_ || !canEditLayers()) return false;
    return pixelClipboard_.has_value() || QApplication::clipboard()->mimeData()->hasImage() || hasLayerClipboard();
}

bool EditorSession::hasPixelsToPaste() const {
    if (!document_ || !canEditLayers()) return false;
    return pixelClipboard_.has_value() || QApplication::clipboard()->mimeData()->hasImage();
}

void EditorSession::paste() {
    if (refusedAtDepth("edit.clipboard", tr("Editing pixels"))) return;
    if (!canPaste()) return;
    if (hasLayerClipboard()) { QString why; pasteLayers(&why); if (!why.isEmpty()) emit error(why); return; }   // EditorSessionClipboard.cpp
    QString why;
    if (!pastePixels(&why) && !why.isEmpty()) emit error(why);
}

bool EditorSession::pastePixels(QString* errorText) {
    if (refusedAtDepth("edit.clipboard", tr("Editing pixels"))) return false;
    if (!hasPixelsToPaste()) return false;
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    QImage external = mime->hasImage() ? qvariant_cast<QImage>(mime->imageData()) : QImage();
    const std::vector<uint64_t> before = historyRevisions();
    const QString unconvertible = tr("The pixels could not be converted to this document's colour mode.");
    // Pixels copied here go back exactly where they came from unless another app copied since.
    if (pixelClipboard_ && (!mime->hasImage() || (external.width() == pixelClipboard_->image.width() && external.height() == pixelClipboard_->image.height()))) {
        // With a single channel as the target, the pixels' gray goes into it (EditorSessionChannels.cpp); the gray is
        // read from sRGB when they came from a CMYK or Lab document.
        const PixelClipboard clip = *pixelClipboard_;
        const AnyImage gray = clip.mode == ColorMode::RGB ? clip.image : convertImage(clip.image, clip.mode, clip.profile, ColorMode::RGB, ColorProfile());
        if (pasteIntoChannels(gray, clip.origin)) return true;
        // Pixels from a document of another mode are converted through the profiles (EditorSessionModes.cpp).
        const AnyImage pixels = pixelsForDocument(clip.image, clip.mode, clip.profile);
        if (!pixels) { if (errorText) *errorText = unconvertible; return false; }
        addPixelLayer(pixels, clip.origin, QT_TRANSLATE_NOOP("History", "Paste"), true);
        return historyRevisions() != before;
    }
    if (external.isNull()) return false;
    QPointF origin(std::floor((document_->width - external.width()) / 2.0), std::floor((document_->height - external.height()) / 2.0));
    if (pasteIntoChannels(fromQImage(external), origin)) return true;
    // Another app's pixels are sRGB: into a CMYK or Lab document through the profiles.
    const AnyImage pixels = pixelsForDocument(fromQImage(external), ColorMode::RGB, ColorProfile());
    if (!pixels) { if (errorText) *errorText = unconvertible; return false; }
    addPixelLayer(pixels, origin, QT_TRANSLATE_NOOP("History", "Paste"), true);
    return historyRevisions() != before;
}

void EditorSession::layerViaCopy() {
    if (refusedAtDepth("edit.clipboard", tr("Editing pixels"))) return;
    if (!canEditLayers()) return;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup) return;
    if (!document_->selection) { duplicateActiveLayer(); return; }
    auto copied = renderSelectedPixels(false);
    if (!copied) return;
    addPixelLayer(copied->image, copied->origin, QT_TRANSLATE_NOOP("History", "Layer via Copy"), false);
}

void EditorSession::addPixelLayer(AnyImage image, QPointF origin, const QString& editName, bool dropsSelection) {
    if (!document_ || !image) return;
    if (const BudgetCheck check = document_->canInsertImage(image.width(), image.height()); !check) { emit error(budgetText(check)); return; }
    // Pixels copied from a document of the other depth (or another app, at 8 bits) take this one's.
    if (document_->sampleType == SampleType::F32) {
        // Into a 32-bit document: linearised through the document's own encoding.
        const TransferCurve curve = encodedTransfer(*document_);
        image = imageAtDepth(image, SampleType::F32, &curve);
    } else if (document_->colorMode != ColorMode::RGB) {
        // CMYK and Lab: already in the mode (pixelsForDocument); the depth changed keeping Lab's neutral a and b.
        image = imageAtFormat(image, document_->sampleType, document_->colorMode);
        if (!image) { emit error(tr("The pixels could not be converted to this document's colour mode.")); return; }
    } else image = imageAtDepth(image, document_->sampleType);
    Layer layer(modeAsset(image, nextLayerName(document_->layers, QCoreApplication::translate("Names", "Layer").toStdString())), toPoint(origin));
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    endOpacityEdit();
    beginEdit(editName);
    document_->layers.insert(document_->layers.begin() + index, layer);
    if (dropsSelection) document_->selection.reset();
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
    if (dropsSelection) emit selectionChanged();
}

AnyImage EditorSession::contentAwareFillResult(const ContentFillRequest& request, LayerTransform& placed, QString* errorText) const {
    if (!canAdjustPixels() || !document_->selection || !document_->selection->coverage) { if (errorText) *errorText = tr("Select a visible image layer and an area to fill."); return nullptr; }
    const Layer* layer = activeLayer();
    // The layer grows over any of the selection on the canvas past its edge.
    Rect area = document_->selection->bounds().intersection(document_->rect());
    if (layer->asset->image.u16()) {
        // At 16 bits: the same steps on the layer's 16-bit pixels (the fill votes from 16-bit texture, inpaint.h).
        const Image16& src = *layer->asset->image.u16();
        Affine toPixels = layer->transform.pixelToDocument(src.width(), src.height()).inverted();
        Rect wanted = toPixels.mapBounds(area).integral().unionWith(Rect(0, 0, src.width(), src.height()));
        int margin = int(std::ceil(std::max({0.0, -wanted.minX(), -wanted.minY(), wanted.maxX() - src.width(), wanted.maxY() - src.height()})));
        LayerTransform grown;
        auto source = adjustmentSource16(margin, grown);
        if (!source) { if (errorText) *errorText = tr("The layer is too large to grow."); return nullptr; }
        auto coverage = selectionOnGrid16(grown, source->width(), source->height());
        if (!coverage) { if (errorText) *errorText = tr("Select an area to fill."); return nullptr; }
        auto out = std::make_shared<Image16>(*source);
        std::shared_ptr<Gray16> visible;
        const Gray16* m = layer->mask && layer->mask->enabled && !layer->mask->placement ? layer->mask->asset.image.u16().get() : nullptr;
        if (m && m->width() == src.width() && m->height() == src.height()) {
            visible = std::make_shared<Gray16>(source->width(), source->height(), uint16_t(one16));
            for (int y = 0; y < m->height(); y++) std::memcpy(visible->row(y + margin) + margin, m->row(y), size_t(m->width()) * sizeof(uint16_t));
        }
        InpaintOptions options;
        if (request.sampling == ContentFillRequest::Sampling::All) {
            if (!visible) visible = std::make_shared<Gray16>(source->width(), source->height(), uint16_t(one16));
            options.sampleWholeVisible = true;
        }
        if (!contentFill(*out, *coverage, options, visible.get())) {
            if (errorText) *errorText = tr("Not enough unselected, opaque image pixels to synthesize a fill. Use a smaller selection with some surrounding image.");
            return nullptr;
        }
        if (request.newLayer)
            for (int y = 0; y < out->height(); y++)
                for (int x = 0; x < out->width(); x++) {
                    uint16_t* p = out->pixel(x, y);
                    const uint32_t c = std::min<uint32_t>(coverage->at(x, y), one16);
                    for (int k = 0; k < 4; k++) p[k] = uint16_t(mul15(p[k], c));
                }
        return Image16Ptr(trimToPixels(*out, grown, placed));
    }
    const Image& src = *layer->asset->image.u8();
    Affine toPixels = layer->transform.pixelToDocument(src.width(), src.height()).inverted();
    Rect wanted = toPixels.mapBounds(area).integral().unionWith(Rect(0, 0, src.width(), src.height()));
    int margin = int(std::ceil(std::max({0.0, -wanted.minX(), -wanted.minY(), wanted.maxX() - src.width(), wanted.maxY() - src.height()})));
    LayerTransform grown;
    auto source = adjustmentSource(margin, grown);
    if (!source) { if (errorText) *errorText = tr("The layer is too large to grow."); return nullptr; }
    auto coverage = selectionOnGrid(grown, source->width(), source->height());
    if (!coverage) { if (errorText) *errorText = tr("Select an area to fill."); return nullptr; }
    auto out = std::make_shared<Image>(*source);
    // What the layer's mask hides is not copied from: a fill at the edge of a cut-out takes the subject.
    std::shared_ptr<GrayImage> visible;
    if (layer->mask && layer->mask->enabled && !layer->mask->placement && layer->mask->asset.image.u8() && layer->mask->asset.image.u8()->width() == src.width() && layer->mask->asset.image.u8()->height() == src.height()) {
        visible = std::make_shared<GrayImage>(source->width(), source->height(), 255);
        const GrayImage& m = *layer->mask->asset.image.u8();
        for (int y = 0; y < m.height(); y++) std::memcpy(visible->row(y + margin) + margin, m.row(y), size_t(m.width()));
    }
    InpaintOptions options;
    if (request.sampling == ContentFillRequest::Sampling::All) {
        // The wider window, inside what the layer's mask shows.
        if (!visible) visible = std::make_shared<GrayImage>(source->width(), source->height(), 255);
        options.sampleWholeVisible = true;
    }
    if (!contentFill(*out, *coverage, options, visible.get())) {
        if (errorText) *errorText = tr("Not enough unselected, opaque image pixels to synthesize a fill. Use a smaller selection with some surrounding image.");
        return nullptr;
    }
    if (request.newLayer) {
        // Only the filled pixels, by the selection's coverage, for a layer of their own.
        for (int y = 0; y < out->height(); y++)
            for (int x = 0; x < out->width(); x++) {
                uint8_t* p = out->pixel(x, y);
                const int c = coverage->at(x, y);
                for (int k = 0; k < 4; k++) p[k] = uint8_t((p[k] * c + 127) / 255);
            }
    }
    return std::shared_ptr<const Image>(trimToPixels(*out, grown, placed));
}

bool EditorSession::contentAwareFill(QString* errorText, const ContentFillRequest& request) {
    if (refusedAtDepth("edit.contentAware", tr("Content-aware editing"), errorText)) return false;
    LayerTransform placed;
    auto result = contentAwareFillResult(request, placed, errorText);
    if (!result) return false;
    if (!request.newLayer) { commitPixels(result, placed, QT_TRANSLATE_NOOP("History", "Content-Aware Fill")); return true; }
    if (const BudgetCheck check = document_->canInsertImage(result.width(), result.height()); !check) { if (errorText) *errorText = budgetText(check); return false; }
    const Layer* active = activeLayer();
    Layer layer(Asset::makeAny(result, nextLayerName(document_->layers, QCoreApplication::translate("Names", "Layer").toStdString())), Point{0, 0});
    layer.transform = placed;
    layer.parentId = active ? active->parentId : std::nullopt;
    const int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    clearPixelPreview();
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Content-Aware Fill"));
    document_->layers.insert(document_->layers.begin() + index, layer);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::contentAwareMove(int dx, int dy, QString* errorText) {
    if (refusedAtDepth("edit.contentAware", tr("Content-aware editing"), errorText)) return false;
    auto failWith = [&](const QString& text) { if (errorText) *errorText = text; return false; };
    if (!canAdjustPixels() || !document_->selection || !document_->selection->coverage) return failWith(tr("Select an area on a visible image layer, then drag it where it should go."));
    if (dx == 0 && dy == 0) return failWith(tr("Drag the selection to where it should go."));
    if (isMaskSelected_) return failWith(tr("Content-Aware Move works on a layer's pixels, not its mask."));
    if (smartObjectBlocksPixels(true)) return false;
    const Layer* layer = activeLayer();
    // The layer grows over the selection and where it lands.
    Rect sel = document_->selection->bounds();
    Rect area = sel.unionWith(Rect(sel.x + dx, sel.y + dy, sel.width, sel.height)).intersection(document_->rect());
    const QString name = contentMoveExtend ? QStringLiteral(QT_TRANSLATE_NOOP("History", "Content-Aware Extend")) : QStringLiteral(QT_TRANSLATE_NOOP("History", "Content-Aware Move"));
    if (layer->asset->image.u16()) {
        // At 16 bits: the same steps on the 16-bit pixels and selection (contentmove.h).
        const Image16& src = *layer->asset->image.u16();
        Affine toPixels = layer->transform.pixelToDocument(src.width(), src.height()).inverted();
        Rect wanted = toPixels.mapBounds(area).integral().unionWith(Rect(0, 0, src.width(), src.height()));
        int margin = int(std::ceil(std::max({0.0, -wanted.minX(), -wanted.minY(), wanted.maxX() - src.width(), wanted.maxY() - src.height()})));
        LayerTransform grown;
        auto source = adjustmentSource16(margin, grown);
        if (!source) return failWith(tr("The layer is too large to grow."));
        auto coverage = selectionOnGrid16(grown, source->width(), source->height());
        if (!coverage) return failWith(tr("Select the area to move first."));
        Affine gridFromDoc = grown.pixelToDocument(source->width(), source->height()).inverted();
        const Point o = gridFromDoc.apply({0, 0}), d = gridFromDoc.apply({double(dx), double(dy)});
        const int gdx = int(std::lround(d.x - o.x)), gdy = int(std::lround(d.y - o.y));
        std::shared_ptr<Gray16> visible;
        const Gray16* m = layer->mask && layer->mask->enabled && !layer->mask->placement ? layer->mask->asset.image.u16().get() : nullptr;
        if (m && m->width() == src.width() && m->height() == src.height()) {
            visible = std::make_shared<Gray16>(source->width(), source->height(), uint16_t(one16));
            for (int y = 0; y < m->height(); y++) std::memcpy(visible->row(y + margin) + margin, m->row(y), size_t(m->width()) * sizeof(uint16_t));
        }
        auto out = std::make_shared<Image16>(*source);
        ContentMoveOptions options;
        options.extend = contentMoveExtend;
        options.adaptation = contentMoveAdaptation;
        if (!compositor::contentAwareMove(*out, *coverage, gdx, gdy, options, visible.get()))
            return failWith(tr("Nothing could be moved there: the selection must land on the layer and leave opaque pixels around it to fill from."));
        LayerTransform placed;
        auto trimmed = trimToPixels(*out, grown, placed);
        beginEdit(name);
        commitPixels(Image16Ptr(trimmed), placed, name);
        document_->selection = offsetSelection(*document_->selection, dx, dy);
        endEdit();
        notifyDocument();
        emit selectionChanged();
        return true;
    }
    const Image& src = *layer->asset->image.u8();
    Affine toPixels = layer->transform.pixelToDocument(src.width(), src.height()).inverted();
    Rect wanted = toPixels.mapBounds(area).integral().unionWith(Rect(0, 0, src.width(), src.height()));
    int margin = int(std::ceil(std::max({0.0, -wanted.minX(), -wanted.minY(), wanted.maxX() - src.width(), wanted.maxY() - src.height()})));
    LayerTransform grown;
    auto source = adjustmentSource(margin, grown);
    if (!source) return failWith(tr("The layer is too large to grow."));
    auto coverage = selectionOnGrid(grown, source->width(), source->height());
    if (!coverage) return failWith(tr("Select the area to move first."));
    // The drag on the layer's own grid.
    Affine gridFromDoc = grown.pixelToDocument(source->width(), source->height()).inverted();
    const Point o = gridFromDoc.apply({0, 0}), d = gridFromDoc.apply({double(dx), double(dy)});
    const int gdx = int(std::lround(d.x - o.x)), gdy = int(std::lround(d.y - o.y));
    std::shared_ptr<GrayImage> visible;
    if (layer->mask && layer->mask->enabled && !layer->mask->placement && layer->mask->asset.image.u8() && layer->mask->asset.image.u8()->width() == src.width() && layer->mask->asset.image.u8()->height() == src.height()) {
        visible = std::make_shared<GrayImage>(source->width(), source->height(), 255);
        const GrayImage& m = *layer->mask->asset.image.u8();
        for (int y = 0; y < m.height(); y++) std::memcpy(visible->row(y + margin) + margin, m.row(y), size_t(m.width()));
    }
    auto out = std::make_shared<Image>(*source);
    ContentMoveOptions options;
    options.extend = contentMoveExtend;
    options.adaptation = contentMoveAdaptation;
    if (!compositor::contentAwareMove(*out, *coverage, gdx, gdy, options, visible.get()))
        return failWith(tr("Nothing could be moved there: the selection must land on the layer and leave opaque pixels around it to fill from."));
    LayerTransform placed;
    auto trimmed = trimToPixels(*out, grown, placed);
    // The selection follows the patch, as in Photoshop.
    const GrayImage& before = *document_->selection->coverage.u8();
    auto shifted = std::make_shared<GrayImage>(before.width(), before.height());
    for (int y = 0; y < shifted->height(); y++) {
        const int sy = y - dy;
        if (sy < 0 || sy >= before.height()) continue;
        for (int x = std::max(0, dx); x < std::min(shifted->width(), shifted->width() + dx); x++) shifted->at(x, y) = before.at(x - dx, sy);
    }
    beginEdit(name);
    commitPixels(trimmed, placed, name);
    Selection moved = *document_->selection;
    moved.coverage = shifted;
    document_->selection = moved;
    endEdit();
    notifyDocument();
    emit selectionChanged();
    return true;
}

bool EditorSession::contentAwareScale(int width, int height, bool protectSelection, QString* errorText) {
    if (refusedAtDepth("edit.contentAware", tr("Content-aware editing"), errorText)) return false;
    if (!canAdjustPixels()) { if (errorText) *errorText = tr("Select a visible image layer to scale."); return false; }
    Layer* layer = activeLayerMutable();
    if (const Image16Ptr deep = layer->asset->image.u16()) {
        // At 16 bits: seams chosen on the pixels rounded to 8 bits, the 16-bit pixels carried (seamcarve.h).
        if (!Document::validDimension(width) || !Document::validDimension(height) || (long long)width * height > document_->imagePixelBudget()) {
            if (errorText) *errorText = tr("The size must be between 1 and %1 pixels a side, %2 megapixels at most.").arg(maxImageSide).arg(document_->imagePixelBudget() / 1000000);
            return false;
        }
        std::shared_ptr<GrayImage> protect;
        if (protectSelection && document_->selection && document_->selection->coverage.u16())
            protect = narrowGray(*selectionOnGrid16(layer->transform, deep->width(), deep->height()));
        SeamCarveOptions options;
        options.protect = protect.get();
        auto out = std::make_shared<Image16>(seamCarve(*deep, width, height, options));
        if (out->isEmpty()) { if (errorText) *errorText = tr("Could not scale the layer."); return false; }
        LayerTransform placed = layer->transform;
        placed.size = {placed.size.width * width / deep->width(), placed.size.height * height / deep->height()};
        commitPixels(Image16Ptr(out), placed, QT_TRANSLATE_NOOP("History", "Content-Aware Scale"));
        return true;
    }
    const ImagePtr src = layer->asset->image.u8();
    if (const BudgetCheck check = Document::canCreate(width, height, document_->sampleType); !check) { if (errorText) *errorText = budgetText(check); return false; }
    std::shared_ptr<GrayImage> protect;
    if (protectSelection && document_->selection && document_->selection->coverage) protect = selectionOnGrid(layer->transform, src->width(), src->height());
    SeamCarveOptions options;
    options.protect = protect.get();
    auto out = std::make_shared<Image>(seamCarve(*src, width, height, options));
    if (out->isEmpty()) { if (errorText) *errorText = tr("Could not scale the layer."); return false; }
    LayerTransform placed = layer->transform;
    placed.size = {placed.size.width * width / src->width(), placed.size.height * height / src->height()};
    commitPixels(out, placed, QT_TRANSLATE_NOOP("History", "Content-Aware Scale"));
    return true;
}

bool EditorSession::copyLayerFrom(const EditorSession& source, const Uuid& id, std::optional<QPointF> at, QString* errorText) {
    if (!source.document_ || (document_ && !canEditLayers())) return false;
    const Document& from = *source.document_;
    if (!from.find(id)) return false;
    std::set<Uuid> included = descendantIds(from.layers, id);
    included.insert(id);
    std::vector<Layer> copied;
    for (auto& l : from.layers) if (included.count(l.id)) copied.push_back(l);
    long long added = 0, addedMasks = 0;
    for (auto& l : copied) {
        if (l.asset && l.asset->image) added += (long long)l.asset->image.width() * l.asset->image.height();
        if (l.mask && l.mask->asset.image) addedMasks += (long long)l.mask->asset.image.width() * l.mask->asset.image.height();
    }
    // The budgets in bytes, at the receiving document's depth (a new one takes the source's size, at 8 bits).
    {
        Document empty(from.width, from.height);
        const Document& into = document_ ? *document_ : empty;
        BudgetCheck check = document_ ? BudgetCheck{} : Document::canCreate(from.width, from.height, SampleType::U8);
        if (check) check = into.canAddLayers((long long)copied.size(), added, addedMasks);
        if (!check) { if (errorText) *errorText = budgetText(check); return false; }
    }
    // Clipping to a layer that stays behind is baked into the pixels.
    for (auto& l : copied) {
        if (l.maskSourceId && !included.count(*l.maskSourceId)) {
            if (!l.adjustment) { if (auto baked = source.bakeClipping(l.id)) l.asset = *baked; }
            l.maskSourceId.reset();
        }
    }
    std::map<Uuid, Uuid> mapping;
    for (auto& l : copied) mapping[l.id] = makeUuid();
    commitTransform();
    resolveGradient();
    beginEdit(QT_TRANSLATE_NOOP("History", "Copy Layer"));
    if (!document_) {
        document_ = Document(from.width, from.height);
        document_->resolution = from.resolution;
        viewport.fit({double(from.width), double(from.height)});
        emit viewportChanged();
        emit projectPathChanged();
    }
    const Layer* dragged = from.find(id);
    Point anchor = dragged->transform.center();
    Point center = at ? toPoint(*at) : Point(document_->width / 2.0, document_->height / 2.0);
    double dx = center.x - anchor.x, dy = center.y - anchor.y;
    const Layer* active = activeLayer();
    std::optional<Uuid> parent = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    std::vector<Layer> placed;
    for (auto& l : copied) {
        Layer c = l;
        c.id = mapping[l.id];
        c.parentId = l.parentId && mapping.count(*l.parentId) ? std::optional(mapping[*l.parentId]) : parent;
        if (c.maskSourceId) c.maskSourceId = mapping.count(*c.maskSourceId) ? std::optional(mapping[*c.maskSourceId]) : std::nullopt;
        c.transform.origin.x += dx; c.transform.origin.y += dy;
        if (c.mask && c.mask->placement) { c.mask->placement->origin.x += dx; c.mask->placement->origin.y += dy; }
        placed.push_back(c);
    }
    document_->layers.insert(document_->layers.begin() + std::min(index, int(document_->layers.size())), placed.begin(), placed.end());
    setActiveLayer(mapping[id]);
    if (parent) collapsedGroupIds.erase(*parent);
    endEdit();
    notifyDocument();
    emit selectionChanged();
    return true;
}

} // namespace app
