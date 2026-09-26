// Smart Filter editing, as in Photoshop's Layers panel: an entry's settings, switches, blending, order and removal,
// the whole stack on or off, and the shared filter mask. Each change is one undo step through the core's
// setSmartFilters, which writes the placement's 'filterFX' and the document's 'FEid' record anew (so a PSD export
// carries it) and draws the instance again.
//
// The filter mask is painted the way Quick Mask is: a temporary layer at the top holds it as its layer mask, active
// with the mask selected, so every mask tool (brushes, fills, gradients, filters, Invert) works on it; endEdit writes
// the mask into the stack inside the same step. Selecting another layer, saving or exporting takes the layer away.
#include "EditorSession.h"
#include "compositor/render.h"
#include <algorithm>

using namespace compositor;

namespace app {

namespace {

/// The stack's mask over the whole document (its tone past its bounds where it has none).
std::shared_ptr<GrayImage> documentMask(const SmartFilterStack& stack, int w, int h) {
    auto out = std::make_shared<GrayImage>(w, h, stack.maskDefault);
    if (!stack.mask) return out;
    const PixelRect& b = stack.maskBounds;
    for (int y = std::max(0, b.y); y < std::min(h, b.y + stack.mask->height()); y++)
        for (int x = std::max(0, b.x); x < std::min(w, b.x + stack.mask->width()); x++) out->at(x, y) = stack.mask->at(x - b.x, y - b.y);
    return out;
}

} // namespace

std::optional<SmartFilterStack> EditorSession::smartFilters(const Uuid& id) const {
    const Layer* layer = document_ ? document_->find(id) : nullptr;
    return layer ? smartFilterStackOf(*document_, *layer) : std::nullopt;
}

bool EditorSession::canEditSmartFilters(const Uuid& id) const {
    const Layer* layer = document_ ? document_->find(id) : nullptr;
    if (!canEditLayers() || !layer || !layer->isLiveSmartObject() || layer->smartObject->locked()) return false;
    auto stack = smartFilterStackOf(*document_, *layer);
    return stack && stack->supported;
}

bool EditorSession::setSmartFilters(const Uuid& id, const SmartFilterStack& stack, const QString& name, QString* error) {
    Layer* layer = document_ ? document_->find(id) : nullptr;
    if (!canEditLayers() || !layer) { if (error) *error = tr("Select a smart object."); return false; }
    endOpacityEdit();
    const Layer before = *layer;
    const auto carry = document_->psdCarry;
    beginEdit(name);
    std::string why;
    if (!compositor::setSmartFilters(*document_, *layer, stack, &why)) {
        *layer = before;
        document_->psdCarry = carry;
        endEdit();
        if (error) *error = QString::fromStdString(why);
        return false;
    }
    endEdit();
    notifyDocument();
    return true;
}

namespace {
// The stack for an edit on `id`, or an error.
std::optional<SmartFilterStack> editable(const EditorSession& s, const Uuid& id, int index, QString* error) {
    auto stack = s.smartFilters(id);
    if (!stack) { if (error) *error = QObject::tr("That layer has no Smart Filters."); return std::nullopt; }
    if (!s.canEditSmartFilters(id)) {
        if (error) *error = QObject::tr("These Smart Filters include one NekoPhoto does not draw (or the smart object is locked); they cannot be changed here.");
        return std::nullopt;
    }
    if (index < -1 || index >= int(stack->entries.size())) { if (error) *error = QObject::tr("No Smart Filter at index %1.").arg(index); return std::nullopt; }
    return stack;
}
} // namespace

bool EditorSession::setSmartFilterEntry(const Uuid& id, int index, const SmartFilterEntry& entry, QString* error) {
    auto stack = editable(*this, id, index, error);
    if (!stack) return false;
    if (index < 0) { if (error) *error = tr("Name a Smart Filter."); return false; }
    stack->entries[size_t(index)] = entry;
    return setSmartFilters(id, *stack, tr("Smart Filter Settings"), error);
}

bool EditorSession::setSmartFilterEnabled(const Uuid& id, int index, bool enabled, QString* error) {
    auto stack = editable(*this, id, index, error);
    if (!stack) return false;
    if (index < 0) stack->enabled = enabled;
    else stack->entries[size_t(index)].enabled = enabled;
    return setSmartFilters(id, *stack, enabled ? tr("Enable Smart Filter") : tr("Disable Smart Filter"), error);
}

bool EditorSession::moveSmartFilter(const Uuid& id, int from, int to, QString* error) {
    auto stack = editable(*this, id, from, error);
    if (!stack) return false;
    if (from < 0 || to < 0 || to >= int(stack->entries.size())) { if (error) *error = tr("No place %1 in the stack.").arg(to); return false; }
    if (from == to) return true;
    SmartFilterEntry moved = stack->entries[size_t(from)];
    stack->entries.erase(stack->entries.begin() + from);
    stack->entries.insert(stack->entries.begin() + to, moved);
    return setSmartFilters(id, *stack, tr("Move Smart Filter"), error);
}

bool EditorSession::removeSmartFilter(const Uuid& id, int index, QString* error) {
    auto stack = editable(*this, id, index, error);
    if (!stack) return false;
    if (index < 0) { if (error) *error = tr("Name a Smart Filter."); return false; }
    stack->entries.erase(stack->entries.begin() + index);
    if (stack->entries.empty() && filterMaskOwner() == id) endFilterMaskEdit();
    return setSmartFilters(id, *stack, tr("Delete Smart Filter"), error);
}

bool EditorSession::clearSmartFilters(const Uuid& id, QString* error) {
    auto stack = editable(*this, id, -1, error);
    if (!stack) return false;
    if (filterMaskOwner() == id) endFilterMaskEdit();
    return setSmartFilters(id, SmartFilterStack{}, tr("Clear Smart Filters"), error);
}

bool EditorSession::smartFilterMask(const Uuid& id, FilterMaskAction action, QString* error) {
    // A mask being painted is written first, so the action works on what the canvas shows.
    auto stack = editable(*this, id, -1, error);
    if (!stack) return false;
    const bool painting = filterMaskOwner() == id;
    QString name;
    switch (action) {
    case FilterMaskAction::Enable: stack->maskEnabled = true; name = tr("Enable Filter Mask"); break;
    case FilterMaskAction::Disable: stack->maskEnabled = false; name = tr("Disable Filter Mask"); break;
    case FilterMaskAction::Delete:
        if (painting) endFilterMaskEdit();
        stack = smartFilters(id);
        stack->mask.reset();
        stack->maskBounds = {};
        stack->maskDefault = 255;
        stack->maskEnabled = true;
        name = tr("Delete Filter Mask");
        break;
    case FilterMaskAction::Invert: {
        if (painting) { invertMask(); return true; }   // the proxy's mask, synced by endEdit
        auto inverted = documentMask(*stack, document_->width, document_->height);
        for (size_t i = 0; i < inverted->byteCount(); i++) inverted->data()[i] = uint8_t(255 - inverted->data()[i]);
        stack->mask = inverted;
        stack->maskBounds = {0, 0, document_->width, document_->height};
        stack->maskDefault = uint8_t(255 - stack->maskDefault);
        name = tr("Invert Filter Mask");
        break;
    }
    }
    return setSmartFilters(id, *stack, name, error);
}

// ---- Painting the filter mask ---------------------------------------------------------------------------

std::optional<Uuid> EditorSession::filterMaskOwner() const {
    if (!document_ || !filterMaskLayer_ || !filterMaskOwner_) return std::nullopt;
    if (!document_->find(*filterMaskLayer_) || !document_->find(*filterMaskOwner_)) return std::nullopt;
    return filterMaskOwner_;
}

bool EditorSession::beginFilterMaskEdit(const Uuid& id, bool show, QString* error) {
    if (filterMaskOwner() == id) {
        if (activeLayerId_ != filterMaskLayer_) { setActiveLayer(filterMaskLayer_); isMaskSelected_ = true; emit layersChanged(); }
        setFilterMaskShown(show);
        return true;
    }
    auto stack = editable(*this, id, -1, error);
    if (!stack) return false;
    endTemporaryLayers();
    if (document_->layers.size() >= size_t(Document::maxLayers)) { if (error) *error = tr("The document has too many layers."); return false; }
    const int w = document_->width, h = document_->height;
    auto mask = documentMask(*stack, w, h);
    beginEdit("Edit Filter Mask");
    Layer layer(Asset::make(std::make_shared<Image>(w, h), "Smart Filter Mask"), Point(0, 0));
    LayerMask m;
    m.asset = MaskAsset::make(mask);
    layer.mask = m;
    filterMaskLayer_ = layer.id;
    filterMaskOwner_ = id;
    filterMaskSynced_ = layer.mask->asset.image;
    filterMaskShown_ = show;
    document_->layers.push_back(layer);
    setActiveLayer(layer.id);
    isMaskSelected_ = true;
    endEdit();
    notifyDocument();
    emit notice(tr("Painting the Smart Filters' mask: select a layer to finish"));
    return true;
}

bool EditorSession::endFilterMaskEdit() {
    if (!document_ || !filterMaskLayer_ || !document_->find(*filterMaskLayer_)) {
        // Not on (or taken away by an undo).
        filterMaskShown_ = false;
        return false;
    }
    const std::optional<Uuid> owner = filterMaskOwner_ && document_->find(*filterMaskOwner_) ? filterMaskOwner_ : std::nullopt;
    const Uuid proxy = *filterMaskLayer_;
    beginEdit("Exit Filter Mask");
    document_->layers.erase(std::remove_if(document_->layers.begin(), document_->layers.end(), [&](const Layer& l) { return l.id == proxy; }),
                            document_->layers.end());
    filterMaskSynced_.reset();
    filterMaskShown_ = false;
    setActiveLayer(owner && document_->find(*owner) ? owner : (document_->layers.empty() ? std::nullopt : std::optional<Uuid>(document_->layers.back().id)));
    isMaskSelected_ = false;
    endEdit();
    notifyDocument();
    return true;
}

void EditorSession::setFilterMaskShown(bool shown) {
    if (filterMaskShown_ == shown) return;
    filterMaskShown_ = shown;
    documentRevision_++;
    emit documentChanged({});
    emit layersChanged();
}

void EditorSession::endTemporaryLayers() {
    endFilterMaskEdit();
    endQuickMask();
}

void EditorSession::syncFilterMask() {
    const auto owner = filterMaskOwner();
    if (!owner) return;
    Layer* proxy = document_->find(*filterMaskLayer_);
    Layer* layer = document_->find(*owner);
    if (!proxy->mask || !proxy->mask->asset.image || proxy->mask->asset.image == filterMaskSynced_) return;
    auto stack = smartFilterStackOf(*document_, *layer);
    if (!stack || !stack->supported) return;
    // The mask as it sits on the canvas (it may have been moved).
    const int w = document_->width, h = document_->height;
    const uint8_t background = proxy->mask->placement ? LayerMask::background(*proxy->mask->asset.thumbnail) : stack->maskDefault;
    auto mask = std::make_shared<GrayImage>(w, h, background);
    sampleMaskCoverage(*proxy->mask->asset.image, proxy->maskTransform(), document_->rect(), 1, background, *mask, false);
    stack->mask = mask;
    stack->maskBounds = {0, 0, w, h};
    stack->maskDefault = background;
    const Layer before = *layer;
    const auto carry = document_->psdCarry;
    std::string why;
    if (!compositor::setSmartFilters(*document_, *layer, *stack, &why)) {
        *layer = before;
        document_->psdCarry = carry;
        emit error(QString::fromStdString(why));
    }
    filterMaskSynced_ = proxy->mask->asset.image;
}

} // namespace app
