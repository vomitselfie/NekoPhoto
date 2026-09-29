// EditorSession: Channels (Window ▸ Channels, Select ▸ Save Selection and Load Selection; docs/channels.md).
//
// The colour channels are views: `activeColors_` are the ones edits write to (every edit of a layer's pixels is
// limited to them when the outermost edit ends, restrictToColorChannels) and `visibleColors_` the ones the canvas
// shows. With all three active, which is the usual case, nothing here runs and every edit takes its usual path.
//
// An alpha channel made the target is painted through a temporary layer at the top, as Quick Mask is: the channel's
// colour at its opacity, its mask the inverse of the channel's gray (the colour shows over the channel's black), so
// every mask tool works on it and "white" adds to what the channel selects. Each edit's change to that mask is written
// into the channel in the same undo step. The layer itself is not an undo step (choosing a channel is not one in
// Photoshop); it is hidden from the Layers panel and never written.
#include "EditorSession.h"
#include "ImageConvert.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/render.h"
#include <QClipboard>
#include <QGuiApplication>
#include <algorithm>

using namespace compositor;

namespace app {

namespace {

/// A document-sized gray at the document's depth: `gray` sampled through `transform` (a mask as it sits on the canvas).
AnyGray canvasGray(const Document& document, const LayerMask& mask, const LayerTransform& transform) {
    const int w = document.width, h = document.height;
    const uint8_t background = mask.placement && mask.asset.thumbnail ? LayerMask::background(*mask.asset.thumbnail) : 0;
    if (const GrayFPtr& floating = mask.asset.image.f32()) {
        if (!mask.placement && floating->width() == w && floating->height() == h && transform.samePlacement(LayerTransform(Point(0, 0), document.size()))) return floating;
        auto out = std::make_shared<GrayF>(w, h, background / 255.0f);
        sampleMaskCoverage(*floating, transform, document.rect(), 1, background / 255.0f, *out, false);
        return GrayFPtr(out);
    }
    if (const Gray16Ptr& deep = mask.asset.image.u16()) {
        // Unplaced and already canvas-sized: the mask as it is.
        if (!mask.placement && deep->width() == w && deep->height() == h && transform.samePlacement(LayerTransform(Point(0, 0), document.size()))) return deep;
        auto out = std::make_shared<Gray16>(w, h, widen8(background));
        sampleMaskCoverage(*deep, transform, document.rect(), 1, widen8(background), *out, false);
        return Gray16Ptr(out);
    }
    if (const GrayPtr& eight = mask.asset.image.u8()) {
        if (!mask.placement && eight->width() == w && eight->height() == h && transform.samePlacement(LayerTransform(Point(0, 0), document.size()))) return eight;
        auto out = std::make_shared<GrayImage>(w, h, background);
        sampleMaskCoverage(*eight, transform, document.rect(), 1, background, *out, false);
        return GrayPtr(out);
    }
    return {};
}

AnyGray inverted(const AnyGray& gray) {
    if (const GrayFPtr& g = gray.f32()) {
        auto out = std::make_shared<GrayF>(g->width(), g->height());
        for (int y = 0; y < g->height(); y++) for (int x = 0; x < g->width(); x++) out->at(x, y) = 1.0f - cleanCoverage(g->at(x, y));
        return GrayFPtr(out);
    }
    if (const Gray16Ptr& g = gray.u16()) {
        auto out = std::make_shared<Gray16>(g->width(), g->height());
        for (int y = 0; y < g->height(); y++) for (int x = 0; x < g->width(); x++) out->at(x, y) = uint16_t(one16 - std::min<uint32_t>(g->at(x, y), one16));
        return Gray16Ptr(out);
    }
    if (const GrayPtr& g = gray.u8()) {
        auto out = std::make_shared<GrayImage>(g->width(), g->height());
        for (int y = 0; y < g->height(); y++) for (int x = 0; x < g->width(); x++) out->at(x, y) = uint8_t(255 - g->at(x, y));
        return GrayPtr(out);
    }
    return {};
}

AnyImage solidColorRgb(const Document& document, const std::array<double, 3>& color);

/// The channel's colour as a canvas-sized fill at the document's depth and in its mode (the colour is sRGB in CMYK and
/// Lab documents, as their stored colours are).
AnyImage solidColor(const Document& document, const std::array<double, 3>& color) {
    const AnyImage rgb = solidColorRgb(document, color);
    if (document.colorMode == ColorMode::RGB) return rgb;
    const AnyImage converted = convertImage(rgb, ColorMode::RGB, ColorProfile(), document.colorMode, document.profile);
    return converted ? converted : rgb;
}

AnyImage solidColorRgb(const Document& document, const std::array<double, 3>& color) {
    const int w = document.width, h = document.height;
    if (document.sampleType == SampleType::F32) {
        // The overlay colour (encoded sRGB, as it is stored) linearised through the document's curve.
        const TransferCurve curve = encodedTransfer(document);
        auto pixels = std::make_shared<ImageF>(w, h);
        const float c[4] = {curve.toLinear(float(color[0])), curve.toLinear(float(color[1])), curve.toLinear(float(color[2])), 1.0f};
        pixels->fill(c);
        return ImageFPtr(pixels);
    }
    if (document.sampleType == SampleType::U16) {
        auto pixels = std::make_shared<Image16>(w, h);
        const uint16_t c[4] = {uint16_t(std::lround(color[0] * one16)), uint16_t(std::lround(color[1] * one16)), uint16_t(std::lround(color[2] * one16)), uint16_t(one16)};
        pixels->fill(c);
        return Image16Ptr(pixels);
    }
    auto pixels = std::make_shared<Image>(w, h);
    pixels->fill(uint8_t(std::lround(color[0] * 255)), uint8_t(std::lround(color[1] * 255)), uint8_t(std::lround(color[2] * 255)), 255);
    return ImagePtr(pixels);
}

/// The document without the layers that only stand in for something (Quick Mask, a channel being painted).
Document withoutTemporaryLayers(const Document& document, const std::vector<Uuid>& ids) {
    Document out = document;
    out.layers.erase(std::remove_if(out.layers.begin(), out.layers.end(), [&](const Layer& l) { return std::find(ids.begin(), ids.end(), l.id) != ids.end(); }), out.layers.end());
    return out;
}

} // namespace

void EditorSession::followChannelDocument() {
    // The channel view belongs to one document: another taking the session (New, Open, a recovered file) starts from
    // the composite, as a newly opened document does in Photoshop.
    const std::optional<Uuid> id = document_ ? std::optional<Uuid>(document_->id) : std::nullopt;
    // A mode conversion (Image > Mode) changes which colour channels there are: the composite again.
    const std::optional<ColorMode> mode = document_ ? std::optional<ColorMode>(document_->colorMode) : std::nullopt;
    if (id == channelsFor_ && mode == channelsModeFor_) return;
    channelsFor_ = id;
    channelsModeFor_ = mode;
    const bool changed = activeColors_ != allColors() || visibleColors_ != allColors() || !visibleAlpha_.empty() || channelTarget_ || channelProxy_;
    activeColors_ = visibleColors_ = allColors();
    visibleAlpha_.clear();
    channelTarget_.reset();
    channelProxy_.reset();
    channelSynced_.reset();
    channelEditBase_.reset();
    if (changed) emit channelsChanged();
}

std::optional<Uuid> EditorSession::targetChannel() const {
    if (!document_ || !channelTarget_) return std::nullopt;
    const Channel* channel = findChannel(*document_, *channelTarget_);
    if (!channel) return std::nullopt;
    // An alpha channel is the target while its layer is there (an undo past choosing it takes it away).
    if (channel->kind == ChannelKind::Alpha && !(channelProxy_ && document_->find(*channelProxy_))) return std::nullopt;
    return channelTarget_;
}

void EditorSession::selectColorChannels(unsigned bits, bool extend) {
    bits &= allColors();
    if (!bits || !document_) return;
    endChannelEdit();
    channelTarget_.reset();
    if (extend) {
        activeColors_ |= bits;
        visibleColors_ |= bits;
    } else {
        // As in Photoshop, a channel clicked alone is the only one shown (the composite shows the three).
        activeColors_ = visibleColors_ = bits;
        visibleAlpha_.clear();
    }
    documentRevision_++;
    emit documentChanged({});
    emit channelsChanged();
}

bool EditorSession::selectAlphaChannel(const Uuid& id, bool extend) {
    if (!document_) return false;
    const Channel* channel = findChannel(*document_, id);
    if (!channel) return false;
    if (extend) {
        // Shift-click: shown over the image beside the target, not made the target.
        visibleAlpha_.insert(id);
        documentRevision_++;
        emit documentChanged({});
        emit channelsChanged();
        return true;
    }
    if (channel->kind == ChannelKind::Spot) {
        // Spot channels are shown, not edited, in this version.
        endChannelEdit();
        channelTarget_ = id;
        visibleAlpha_.insert(id);
        documentRevision_++;
        emit documentChanged({});
        emit channelsChanged();
        return true;
    }
    return beginChannelEdit(id);
}

void EditorSession::setColorChannelVisible(unsigned bits, bool visible) {
    bits &= allColors();
    const unsigned next = visible ? (visibleColors_ | bits) : (visibleColors_ & ~bits);
    if (next == visibleColors_) return;
    visibleColors_ = next;
    documentRevision_++;
    emit documentChanged({});
    emit channelsChanged();
}

void EditorSession::setAlphaChannelVisible(const Uuid& id, bool visible) {
    if (visible == bool(visibleAlpha_.count(id))) return;
    if (visible) visibleAlpha_.insert(id); else visibleAlpha_.erase(id);
    // The channel being painted shows through its layer: hiding it ends the editing (Photoshop hides the overlay).
    if (!visible && targetChannel() == id) endChannelEdit();
    documentRevision_++;
    emit documentChanged({});
    emit channelsChanged();
}

ChannelView EditorSession::channelView() const {
    ChannelView view;
    view.color = visibleColors_;
    if (document_) view.mode = document_->colorMode;   // the canvas adds the frame at the document's layout (CMYK, Lab)
    if (!document_ || (document_->channels.empty() && visibleColors_ == allColors())) return view;
    const std::optional<Uuid> target = targetChannel();
    const bool proxied = target && channelProxy_ && findChannel(*document_, *target)->kind == ChannelKind::Alpha;
    std::optional<Uuid> gray;
    if (visibleColors_ == 0) {
        // No colour channel shown: the target in gray, else the first channel shown.
        if (target && visibleAlpha_.count(*target)) gray = target;
        else for (const Channel& c : document_->channels) if (visibleAlpha_.count(c.id)) { gray = c.id; break; }
        if (gray && proxied && gray == target) {
            // The channel as it is being painted: its layer's mask (the inverse), live during a stroke or a gradient.
            const Layer* proxy = document_->find(*channelProxy_);
            AnyGray mask = proxy && proxy->mask ? proxy->mask->asset.image : AnyGray();
            if (stroke_ && strokeMask_ && strokeLayerId_ == *channelProxy_) mask = stroke_->sampleType() == SampleType::U16 ? AnyGray(stroke_->previewMask16()) : AnyGray(stroke_->previewMask());
            else if (gradient_ && gradient_->mask && gradient_->layerId == *channelProxy_)
                mask = gradient_->raster->sampleType() == SampleType::U16 ? AnyGray(gradient_->raster->previewMask16()) : AnyGray(gradient_->raster->previewMask());
            view.gray = mask;
            view.grayInverted = true;
        } else if (gray) view.gray = findChannel(*document_, *gray)->image;
    }
    for (const Channel& c : document_->channels) {
        if (!visibleAlpha_.count(c.id) || c.id == gray || (proxied && c.id == *target)) continue;
        view.overlays.push_back({c.image, c.color, c.opacity});
    }
    return view;
}

// ---- Editing an alpha channel through its layer ----------------------------------------------------------------

bool EditorSession::beginChannelEdit(const Uuid& id) {
    if (refusedAtDepth("edit.channels", tr("Channels"))) return false;
    if (!document_) return false;
    if (targetChannel() == id) return true;
    endChannelEdit();
    if (quickMaskActive()) endQuickMask();
    endFilterMaskEdit();
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers)) return false;
    const Channel* channel = findChannel(*document_, id);
    if (!channel || channel->kind != ChannelKind::Alpha) return false;
    Layer layer(Asset::makeAny(solidColor(*document_, channel->color), channel->name), Point(0, 0));
    layer.opacity = channel->opacity;
    LayerMask mask;
    mask.asset = MaskAsset::makeAny(inverted(grayAtDepth(channel->image, document_->sampleType)));
    layer.mask = mask;
    channelReturnLayer_ = activeLayerId_;
    channelProxy_ = layer.id;
    channelTarget_ = id;
    channelSynced_ = layer.mask->asset.image;
    document_->layers.push_back(layer);
    setActiveLayer(layer.id);
    isMaskSelected_ = true;
    visibleAlpha_.insert(id);
    notifyDocument();
    emit channelsChanged();
    return true;
}

void EditorSession::endChannelEdit() {
    channelTarget_.reset();
    if (!channelProxy_) return;
    const Uuid proxy = *channelProxy_;
    channelProxy_.reset();
    channelSynced_.reset();
    if (!document_ || !document_->find(proxy)) { emit channelsChanged(); return; }
    document_->layers.erase(std::remove_if(document_->layers.begin(), document_->layers.end(), [&](const Layer& l) { return l.id == proxy; }), document_->layers.end());
    setActiveLayer(channelReturnLayer_ && document_->find(*channelReturnLayer_) ? channelReturnLayer_
                   : (document_->layers.empty() ? std::nullopt : std::optional<Uuid>(document_->layers.back().id)));
    isMaskSelected_ = false;
    notifyDocument();
    emit channelsChanged();
}

void EditorSession::syncChannelProxy() {
    if (!document_ || !channelProxy_ || !channelTarget_) return;
    const Layer* proxy = document_->find(*channelProxy_);
    Channel* channel = findChannel(*document_, *channelTarget_);
    if (!proxy || !channel) return;
    // The layer's opacity is the channel's overlay opacity (the Layers panel's slider reaches it while it is active).
    if (proxy->opacity != channel->opacity) { channel->opacity = proxy->opacity; emit channelsChanged(); }
    if (!proxy->mask || !proxy->mask->asset.image || proxy->mask->asset.image == channelSynced_) return;
    // The mask as it sits on the canvas (it may have been moved), inverted: the channel's gray.
    AnyGray gray = inverted(grayAtDepth(canvasGray(*document_, *proxy->mask, proxy->maskTransform()), document_->sampleType));
    if (gray) channel->image = gray;
    channelSynced_ = proxy->mask->asset.image;
    emit channelsChanged();
}

void EditorSession::refreshChannelProxy() {
    if (!document_ || !channelProxy_ || !channelTarget_) return;
    Layer* proxy = document_->find(*channelProxy_);
    const Channel* channel = findChannel(*document_, *channelTarget_);
    if (!proxy || !channel) return;
    proxy->asset = Asset::makeAny(solidColor(*document_, channel->color), channel->name);
    proxy->transform = LayerTransform(Point(0, 0), document_->size());
    proxy->opacity = channel->opacity;
    LayerMask mask;
    mask.asset = MaskAsset::makeAny(inverted(grayAtDepth(channel->image, document_->sampleType)));
    proxy->mask = mask;
    channelSynced_ = proxy->mask->asset.image;
}

// ---- The channels -----------------------------------------------------------------------------------------------

std::optional<Uuid> EditorSession::newChannel(const QString& name, QString* error) {
    if (refusedAtDepth("edit.channels", tr("Channels"), error)) return std::nullopt;
    if (!document_) return std::nullopt;
    if (const std::string why = channelAddProblem(*document_); !why.empty()) {
        if (error) *error = tr("No more channels can be added: %1").arg(QString::fromStdString(why));
        else emit this->error(tr("No more channels can be added: %1").arg(QString::fromStdString(why)));
        return std::nullopt;
    }
    const std::string prefix = QCoreApplication::translate("Names", "Alpha").toStdString();
    Channel channel = makeAlphaChannel(*document_, name.trimmed().isEmpty() ? nextChannelName(*document_, prefix) : name.trimmed().toStdString());
    beginEdit(QT_TRANSLATE_NOOP("History", "New Channel"));
    document_->channels.push_back(channel);
    endEdit();
    emit historyChanged();
    emit titleChanged();
    // The new channel is the target, shown over the image.
    selectAlphaChannel(channel.id);
    emit channelsChanged();
    return channel.id;
}

std::optional<Uuid> EditorSession::duplicateChannel(const Uuid& id, const QString& name, QString* error) {
    if (refusedAtDepth("edit.channels", tr("Channels"), error)) return std::nullopt;
    if (!document_) return std::nullopt;
    const int index = channelIndex(*document_, id);
    if (index < 0) { if (error) *error = tr("There is no such channel."); return std::nullopt; }
    if (const std::string why = channelAddProblem(*document_); !why.empty()) {
        if (error) *error = tr("No more channels can be added: %1").arg(QString::fromStdString(why));
        return std::nullopt;
    }
    Channel copy = document_->channels[size_t(index)];
    copy.id = makeUuid();
    copy.name = name.trimmed().isEmpty() ? tr("%1 copy").arg(QString::fromStdString(copy.name)).toStdString() : name.trimmed().toStdString();
    // A duplicate is a new channel to a PSD (its identifier and stored record are the original's).
    if (copy.psdCarry) {
        auto carry = std::make_shared<PsdChannelCarry>(*copy.psdCarry);
        carry->identifier = 0;
        copy.psdCarry = carry;
    }
    beginEdit(QT_TRANSLATE_NOOP("History", "Duplicate Channel"));
    document_->channels.insert(document_->channels.begin() + index + 1, copy);
    endEdit();
    emit historyChanged();
    emit titleChanged();
    emit channelsChanged();
    return copy.id;
}

bool EditorSession::deleteChannel(const Uuid& id) {
    if (!document_) return false;
    const int index = channelIndex(*document_, id);
    if (index < 0) return false;
    if (channelTarget_ == id) endChannelEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Delete Channel"));
    document_->channels.erase(document_->channels.begin() + index);
    endEdit();
    visibleAlpha_.erase(id);
    notifyDocument();
    emit channelsChanged();
    return true;
}

bool EditorSession::renameChannel(const Uuid& id, const QString& name) {
    if (!document_ || name.trimmed().isEmpty()) return false;
    Channel* channel = findChannel(*document_, id);
    if (!channel || channel->name == name.trimmed().toStdString()) return false;
    beginEdit(QT_TRANSLATE_NOOP("History", "Rename Channel"));
    findChannel(*document_, id)->name = name.trimmed().toStdString();
    endEdit();
    emit historyChanged();
    emit titleChanged();
    emit channelsChanged();
    return true;
}

bool EditorSession::setChannelOptions(const Uuid& id, const QString& name, const QColor& color, double opacity, bool selectedAreas) {
    if (!document_) return false;
    const Channel* before = findChannel(*document_, id);
    if (!before) return false;
    Channel after = *before;
    if (!name.trimmed().isEmpty()) after.name = name.trimmed().toStdString();
    after.color = {color.redF(), color.greenF(), color.blueF()};
    after.opacity = std::clamp(opacity, 0.0, 1.0);
    if (after.kind == ChannelKind::Alpha) setSelectedAreas(after, selectedAreas);
    if (after == *before) return false;
    beginEdit(QT_TRANSLATE_NOOP("History", "Channel Options"));
    *findChannel(*document_, id) = after;
    if (channelTarget_ == id) refreshChannelProxy();
    endEdit();
    notifyDocument();
    emit channelsChanged();
    return true;
}

bool EditorSession::moveChannel(const Uuid& id, int index) {
    if (!document_) return false;
    const int from = channelIndex(*document_, id);
    if (from < 0) return false;
    index = std::clamp(index, 0, int(document_->channels.size()) - 1);
    if (index == from) return false;
    beginEdit(QT_TRANSLATE_NOOP("History", "Channel Order"));
    Channel moved = document_->channels[size_t(from)];
    document_->channels.erase(document_->channels.begin() + from);
    document_->channels.insert(document_->channels.begin() + index, moved);
    endEdit();
    emit historyChanged();
    emit titleChanged();
    emit channelsChanged();
    return true;
}

// ---- Selections and channels ----------------------------------------------------------------------------------

std::optional<Uuid> EditorSession::saveSelectionToChannel(const std::optional<Uuid>& into, const QString& name, SelectionMode mode, QString* error) {
    if (refusedAtDepth("edit.channels", tr("Channels"), error)) return std::nullopt;
    if (!document_) return std::nullopt;
    const std::optional<Selection>& selection = document_->selection;
    if (!selection || !selection->coverage) {
        if (error) *error = tr("There is no selection to save.");
        return std::nullopt;
    }
    if (into) {
        const Channel* channel = findChannel(*document_, *into);
        if (!channel || channel->kind != ChannelKind::Alpha) { if (error) *error = tr("There is no such alpha channel."); return std::nullopt; }
        Channel after = *channel;
        saveSelectionInto(after, selection, mode, document_->sampleType, document_->width, document_->height);
        beginEdit(QT_TRANSLATE_NOOP("History", "Save Selection"));
        *findChannel(*document_, *into) = after;
        if (channelTarget_ == *into) refreshChannelProxy();
        endEdit();
        notifyDocument();
        emit channelsChanged();
        return into;
    }
    if (const std::string why = channelAddProblem(*document_); !why.empty()) {
        if (error) *error = tr("No more channels can be added: %1").arg(QString::fromStdString(why));
        return std::nullopt;
    }
    const std::string prefix = QCoreApplication::translate("Names", "Alpha").toStdString();
    Channel channel = makeAlphaChannel(*document_, name.trimmed().isEmpty() ? nextChannelName(*document_, prefix) : name.trimmed().toStdString(), selection->coverage);
    beginEdit(QT_TRANSLATE_NOOP("History", "Save Selection"));
    document_->channels.push_back(channel);
    endEdit();
    emit historyChanged();
    emit titleChanged();
    emit channelsChanged();
    return channel.id;
}

bool EditorSession::loadSelectionFromSource(const SelectionSource& source, bool invert, SelectionMode mode, QString* error) {
    if (refusedAtDepth("edit.selection", tr("Selections"), error)) return false;
    if (!document_) return false;
    // The layers that stand in for Quick Mask and a channel being painted are not part of the image.
    std::vector<Uuid> temporary;
    if (channelProxy_) temporary.push_back(*channelProxy_);
    if (quickMaskLayer_) temporary.push_back(*quickMaskLayer_);
    const bool composite = source.kind == SelectionSource::Composite || source.kind == SelectionSource::Red || source.kind == SelectionSource::Green
                           || source.kind == SelectionSource::Blue;
    const Document& from = *document_;
    std::string why;
    auto result = composite && !temporary.empty() ? loadSelectionFrom(withoutTemporaryLayers(from, temporary), source, invert, mode, selectionAntialiased, &why)
                                                  : loadSelectionFrom(from, source, invert, mode, selectionAntialiased, &why);
    if (!result) {
        if (error) *error = QString::fromStdString(why);
        return false;
    }
    // A selection that selects nothing is none (Photoshop warns that no pixels are selected and leaves none).
    if (result->isEmpty()) result.reset();
    setSelection(result, QT_TRANSLATE_NOOP("History", "Load Selection"));
    return true;
}

// ---- Paste into channels ----------------------------------------------------------------------------------------

bool EditorSession::pasteIntoChannels(const AnyImage& image, QPointF origin) {
    if (!document_ || !image) return false;
    const bool intoAlpha = targetChannel() && findChannel(*document_, *targetChannel())->kind == ChannelKind::Alpha;
    if (!intoAlpha && activeColors_ == allColors()) return false;
    // The clipboard's gray (luminosity) and coverage, at 8 bits: what a paste into a channel writes.
    const TransferCurve curve = documentCurve();
    const ImagePtr eight = imageAtDepth(image, SampleType::U8, &curve).u8();
    if (!eight) return false;
    const int ox = int(std::lround(origin.x())), oy = int(std::lround(origin.y()));
    auto grayAt = [&](int x, int y, double& value, double& cover) {
        const int sx = x - ox, sy = y - oy;
        cover = 0;
        if (sx < 0 || sy < 0 || sx >= eight->width() || sy >= eight->height()) return;
        const uint8_t* p = eight->pixel(sx, sy);
        if (!p[3]) return;
        cover = p[3] / 255.0;
        value = (0.30 * p[0] + 0.59 * p[1] + 0.11 * p[2]) / p[3];   // straight, 0..1
    };
    if (intoAlpha) {
        Channel* channel = findChannel(*document_, *targetChannel());
        const AnyGray before = grayAtDepth(channel->image, document_->sampleType);
        AnyGray after;
        if (const GrayFPtr& g = before.f32()) {
            auto out = std::make_shared<GrayF>(*g);
            for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
                double v = 0, a = 0;
                grayAt(x, y, v, a);
                if (a > 0) out->at(x, y) = float(out->at(x, y) * (1 - a) + v * a);
            }
            after = GrayFPtr(out);
        } else if (const Gray16Ptr& g = before.u16()) {
            auto out = std::make_shared<Gray16>(*g);
            for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
                double v = 0, a = 0;
                grayAt(x, y, v, a);
                if (a > 0) out->at(x, y) = uint16_t(std::lround(out->at(x, y) * (1 - a) + v * a * one16));
            }
            after = Gray16Ptr(out);
        } else if (const GrayPtr& g = before.u8()) {
            auto out = std::make_shared<GrayImage>(*g);
            for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
                double v = 0, a = 0;
                grayAt(x, y, v, a);
                if (a > 0) out->at(x, y) = uint8_t(std::lround(out->at(x, y) * (1 - a) + v * a * 255));
            }
            after = GrayPtr(out);
        }
        if (!after) return false;
        beginEdit(QT_TRANSLATE_NOOP("History", "Paste"));
        channel = findChannel(*document_, *targetChannel());
        channel->image = after;
        refreshChannelProxy();
        endEdit();
        notifyDocument();
        emit channelsChanged();
        return true;
    }
    // Into the active colour channels of the active layer: the clipboard's gray in every channel over the layer's
    // own alpha, then limited to the active ones by the edit (endEdit).
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || !layer->asset || !layer->asset->image || isMaskSelected_) return false;
    const LayerTransform& t = layer->transform;
    if (t.rotation != 0 || t.flipX || t.flipY || t.size.width != layer->asset->image.width() || t.size.height != layer->asset->image.height()
        || t.origin.x != std::floor(t.origin.x) || t.origin.y != std::floor(t.origin.y)) return false;
    const int lx = int(t.origin.x), ly = int(t.origin.y);
    AnyImage result;
    if (const ImageFPtr& src = layer->asset->image.f32()) {
        // The clipboard's gray is an encoded level: linearised, then mixed in as light.
        auto out = std::make_shared<ImageF>(*src);
        for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
            double v = 0, a = 0;
            grayAt(x + lx, y + ly, v, a);
            float* p = out->pixel(x, y);
            if (a <= 0 || !(p[3] > 0)) continue;
            const double light = curve.toLinear(float(std::min(1.0, v)));
            for (int c = 0; c < 3; c++) p[c] = float((double(p[c]) / p[3] * (1 - a) + light * a) * p[3]);
        }
        result = ImageFPtr(out);
    } else if (const Image16Ptr& src = layer->asset->image.u16()) {
        // RGB and Lab hold alpha fourth, CMYK fifth: the gray goes into every colour sample as stored (a CMYK plate is
        // light where there is no ink, as the Channels panel shows it).
        auto out = std::make_shared<Image16>(*src);
        const int n = out->channels(), alpha = n - 1;
        for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
            double v = 0, a = 0;
            grayAt(x + lx, y + ly, v, a);
            uint16_t* p = out->pixel(x, y);
            if (a <= 0 || !p[alpha]) continue;
            for (int c = 0; c < alpha; c++) {
                const double straight = double(p[c]) / p[alpha];
                p[c] = uint16_t(std::lround((straight * (1 - a) + v * a) * p[alpha]));
            }
        }
        result = Image16Ptr(out);
    } else if (const ImageC8Ptr& src = layer->asset->image.c8()) {
        auto out = std::make_shared<ImageC8>(*src);
        for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
            double v = 0, a = 0;
            grayAt(x + lx, y + ly, v, a);
            uint8_t* p = out->pixel(x, y);
            if (a <= 0 || !p[4]) continue;
            for (int c = 0; c < 4; c++) {
                const double straight = double(p[c]) / p[4];
                p[c] = uint8_t(std::lround((straight * (1 - a) + v * a) * p[4]));
            }
        }
        result = ImageC8Ptr(out);
    } else if (const ImagePtr& src = layer->asset->image.u8()) {
        auto out = std::make_shared<Image>(*src);
        for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
            double v = 0, a = 0;
            grayAt(x + lx, y + ly, v, a);
            uint8_t* p = out->pixel(x, y);
            if (a <= 0 || !p[3]) continue;
            for (int c = 0; c < 3; c++) {
                const double straight = double(p[c]) / p[3];
                p[c] = uint8_t(std::lround((straight * (1 - a) + v * a) * p[3]));
            }
        }
        result = ImagePtr(out);
    }
    if (!result) return false;
    beginEdit(QT_TRANSLATE_NOOP("History", "Paste"));
    Layer* target = activeLayerMutable();
    target->asset = Asset::makeAny(result, target->asset->name);
    endEdit();
    notifyDocument();
    return true;
}

} // namespace app
