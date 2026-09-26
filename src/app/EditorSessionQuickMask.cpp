// Select ▸ Edit in Quick Mask Mode: the selection as a red overlay to paint, as in Photoshop (the colour shows the
// masked areas). While on, a temporary layer at the top holds it: solid red at 50%, its layer mask the inverse of
// the selection, active with the mask selected. So every mask tool works on it (brushes, fills, gradients,
// filters, Invert) and each change is an undo step; painting "white" adds to the selection, as Photoshop's
// painting with white does. Leaving turns the mask back into the selection and takes the layer away; saving and
// exporting leave it first, so it is never written.
#include "EditorSession.h"
#include "compositor/render.h"

using namespace compositor;

namespace app {

bool EditorSession::quickMaskActive() const {
    return quickMaskLayer_ && document_ && document_->find(*quickMaskLayer_);
}

bool EditorSession::paintsQuickMask() const {
    return quickMaskActive() && activeLayerId_ == quickMaskLayer_ && isMaskSelected_;
}

void EditorSession::toggleQuickMask() {
    if (quickMaskActive()) endQuickMask();
    else beginQuickMask();
}

bool EditorSession::beginQuickMask() {
    if (!canEditLayers() || quickMaskActive() || document_->layers.size() >= size_t(Document::maxLayers)) return false;
    const int w = document_->width, h = document_->height;
    auto red = std::make_shared<Image>(w, h);
    red->fill(255, 0, 0, 255);
    // Red over what is not selected; with no selection, nothing is masked yet.
    auto mask = std::make_shared<GrayImage>(w, h, 0);
    if (document_->selection && document_->selection->coverage) {
        const GrayImage& selected = *document_->selection->coverage;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) mask->at(x, y) = uint8_t(255 - selected.at(x, y));
    }
    beginEdit("Quick Mask");
    Layer layer(Asset::make(red, "Quick Mask"), Point(0, 0));
    layer.opacity = 0.5;
    LayerMask m;
    m.asset = MaskAsset::make(mask);
    layer.mask = m;
    quickMaskReturnLayer_ = activeLayerId_;
    quickMaskLayer_ = layer.id;
    document_->layers.push_back(layer);
    document_->selection.reset();
    setActiveLayer(layer.id);
    isMaskSelected_ = true;
    endEdit();
    notifyDocument();
    emit selectionChanged();
    emit notice(tr("Quick Mask: paint white to select, black to mask; Select ▸ Edit in Quick Mask Mode again to finish"));
    return true;
}

bool EditorSession::endQuickMask() {
    if (!quickMaskActive()) { quickMaskLayer_.reset(); return false; }
    const Layer* layer = document_->find(*quickMaskLayer_);
    const int w = document_->width, h = document_->height;
    std::optional<Selection> selection;
    if (layer->mask && layer->mask->asset.image) {
        // The mask as it sits on the canvas (it may have been moved), inverted: what is not masked is selected.
        const uint8_t background = LayerMask::background(*layer->mask->asset.thumbnail);
        auto masked = std::make_shared<GrayImage>(w, h, background);
        sampleMaskCoverage(*layer->mask->asset.image, layer->maskTransform(), document_->rect(), 1, background, *masked, false);
        bool all = true, none = true;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t& v = masked->at(x, y);
                v = uint8_t(255 - v);
                all = all && v == 255;
                none = none && v == 0;
            }
        // Nothing masked (or everything): no selection, as Photoshop leaves it.
        if (!all && !none) { Selection s; s.coverage = masked; s.antialiased = true; selection = s; }
    }
    beginEdit("Exit Quick Mask");
    const Uuid id = *quickMaskLayer_;
    document_->layers.erase(std::remove_if(document_->layers.begin(), document_->layers.end(), [&](const Layer& l) { return l.id == id; }), document_->layers.end());
    document_->selection = selection;
    quickMaskLayer_.reset();
    setActiveLayer(quickMaskReturnLayer_ && document_->find(*quickMaskReturnLayer_) ? quickMaskReturnLayer_
                   : (document_->layers.empty() ? std::nullopt : std::optional<Uuid>(document_->layers.back().id)));
    isMaskSelected_ = false;
    endEdit();
    notifyDocument();
    emit selectionChanged();
    return true;
}

} // namespace app
