// Layer ▸ Layer Style: editing a layer's effects (compositor/layerstyle.h). The style lives in the layer's PSD carry
// as an 'lfx2' block, so drawing, PSD export and the project package need nothing else; a style left as it was keeps
// the file's own bytes.
#include "EditorSession.h"
#include "PresetLibrary.h"

using namespace compositor;

namespace app {

LayerStyle EditorSession::layerStyle(const Uuid& id) const {
    const Layer* layer = document_ ? document_->find(id) : nullptr;
    return layer ? editableLayerStyle(*layer, *document_) : LayerStyle{};
}

bool EditorSession::canStyleLayer(const Uuid& id) const {
    if (!canEditLayers()) return false;
    const Layer* layer = document_->find(id);
    return layer && !layer->adjustment;   // Photoshop gives adjustment layers no effects
}

bool EditorSession::beginLayerStyleEdit(const Uuid& id) {
    if (styleEditLayer_ || !canStyleLayer(id)) return false;
    beginEdit("Layer Style");
    styleEditLayer_ = id;
    styleEditCarry_ = document_->find(id)->psdCarry;
    styleEditDocumentCarry_ = document_->psdCarry;
    return true;
}

void EditorSession::previewLayerStyle(const LayerStyle& style) {
    if (!styleEditLayer_ || !document_) return;
    Layer* layer = document_->find(*styleEditLayer_);
    if (!layer) return;
    // A pattern chosen from the preset library joins the document's patterns, as Photoshop's picker does.
    addDocumentPatterns(*document_, PresetLibrary::instance().patternsFor(style));
    setLayerStyle(*layer, style);
    emit documentChanged({});
}

void EditorSession::endLayerStyleEdit(bool keep) {
    if (!styleEditLayer_) return;
    if (!keep && document_) {
        if (Layer* layer = document_->find(*styleEditLayer_)) layer->psdCarry = styleEditCarry_;
        document_->psdCarry = styleEditDocumentCarry_;
    }
    styleEditLayer_.reset();
    styleEditCarry_.reset();
    styleEditDocumentCarry_.reset();
    endEdit();
    notifyDocument();
}

bool EditorSession::applyLayerStyle(const Uuid& id, const LayerStyle& style) {
    if (styleEditLayer_ || !canStyleLayer(id)) return false;
    beginEdit("Layer Style");
    addDocumentPatterns(*document_, PresetLibrary::instance().patternsFor(style));   // library patterns it names
    setLayerStyle(*document_->find(id), style);
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::applyStylePreset(const Uuid& id, const LayerStyle& style, const std::vector<PatternPreset>& patterns) {
    if (styleEditLayer_ || !canStyleLayer(id)) return false;
    beginEdit("Apply Style");
    addDocumentPatterns(*document_, patterns);
    setLayerStyle(*document_->find(id), style);
    endEdit();
    notifyDocument();
    return true;
}

int EditorSession::addPatterns(const std::vector<PatternPreset>& patterns) {
    if (!canEditLayers() || styleEditLayer_) return 0;
    beginEdit("Add Patterns");
    const int added = addDocumentPatterns(*document_, patterns);
    endEdit();
    if (added) notifyDocument();
    return added;
}

bool EditorSession::activeLayerHasStyle() const {
    return activeLayerId_ && hasAnyEffect(layerStyle(*activeLayerId_));
}

void EditorSession::copyLayerStyle() {
    if (activeLayerHasStyle()) styleClipboard_ = layerStyle(*activeLayerId_);
}

void EditorSession::pasteLayerStyle() {
    if (!styleClipboard_ || !activeLayerId_ || !canStyleLayer(*activeLayerId_)) return;
    beginEdit("Paste Layer Style");
    setLayerStyle(*activeLayerMutable(), *styleClipboard_);
    endEdit();
    notifyDocument();
}

void EditorSession::clearLayerStyle() {
    if (!activeLayerHasStyle() || !canStyleLayer(*activeLayerId_)) return;
    beginEdit("Clear Layer Style");
    setLayerStyle(*activeLayerMutable(), LayerStyle{});
    endEdit();
    notifyDocument();
}

} // namespace app
