// Paths: the Pen and Direct Selection tools and the Paths panel's commands (compositor/vectorlayer.h). The target
// path is the Paths panel's choice, else the active layer's vector mask when it is targeted, else the active vector
// shape layer's path.
#include "EditorSession.h"
#include "QtGeometry.h"
#include "TextLayer.h"
#include "compositor/depth.h"
#include "compositor/selection.h"

using namespace compositor;

namespace app {

// (The outline simplification is core's simplifyLoop, vectorlayer.h.)

namespace {

/// 16-bit coverage scaled by an opacity (Fill Path and Stroke Path paint at the brush's opacity).
std::shared_ptr<Gray16> faded(const Gray16& coverage, double opacity) {
    auto out = std::make_shared<Gray16>(coverage);
    const uint32_t scale = uint32_t(std::lround(std::clamp(opacity, 0.0, 1.0) * 32768));
    if (scale >= 32768) return out;
    const size_t n = size_t(out->width()) * size_t(out->height());
    for (size_t i = 0; i < n; i++) out->data()[i] = uint16_t(mul15(out->data()[i], scale));
    return out;
}

} // namespace

// ---- Pen --------------------------------------------------------------------------------------------------------

void EditorSession::penPress(QPointF p) {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"))) return;
    if (!document_) return;
    const Point at = toPoint(p);
    if (!penDraft_) { penDraft_ = VectorPath::Subpath{}; penDraft_->closed = false; penDraft_->op = VectorPath::Op::Add; }
    penDraft_->knots.push_back({at.x, at.y, at.x, at.y, at.x, at.y});
    emit transformChanged();
}

void EditorSession::penDrag(QPointF p) {
    if (!penDraft_ || penDraft_->knots.empty()) return;
    auto& k = penDraft_->knots.back();
    // The out handle follows the pointer and the in handle mirrors it: a smooth knot.
    k.outX = p.x(); k.outY = p.y();
    k.inX = 2 * k.x - p.x(); k.inY = 2 * k.y - p.y();
    emit transformChanged();
}

void EditorSession::penCancel() {
    if (!penDraft_) return;
    penDraft_.reset();
    emit transformChanged();
}

void EditorSession::penFinish(bool close) {
    if (!penDraft_) return;
    VectorPath::Subpath sub = *penDraft_;
    penDraft_.reset();
    emit transformChanged();
    if (sub.knots.size() < 2 || !canEditLayers()) return;
    sub.closed = close || penMode == PenMode::Shape;   // a shape's outline is always closed
    VectorPath added;
    added.subpaths.push_back(sub);
    if (penMode == PenMode::Path && !vectorMaskTargeted()) {
        // The chosen path, if it is still there (undo may have taken it away); else the Work Path. The new outline
        // is a component of it, combined by the path operation (Combine when none is chosen).
        const bool chosen = activePathId_ && documentPath(*document_, *activePathId_);
        const uint16_t id = chosen ? *activePathId_ : kWorkPathId;
        VectorPath path;
        if (auto existing = documentPath(*document_, id)) path = existing->path;
        addShapeComponent(path, added, pathOp.value_or(VectorPath::Op::Add));
        beginEdit(chosen ? QT_TRANSLATE_NOOP("History", "Add Subpath") : QT_TRANSLATE_NOOP("History", "Work Path"));
        setDocumentPath(*document_, id, "", path);
        endEdit();
        activePathId_ = id;
        notifyDocument();
        emit pathsChanged();
        return;
    }
    if (addComponentToTarget(added, std::nullopt, QT_TRANSLATE_NOOP("History", "Add Subpath"))) return;
    VectorShape shape;
    shape.path = added;
    shape.r = uint8_t(foregroundColor.red()); shape.g = uint8_t(foregroundColor.green()); shape.b = uint8_t(foregroundColor.blue());
    shape.fill = shapeTool.fill || !shapeTool.stroke.enabled;
    shape.fillPaint = shapeTool.fillPaint;
    shape.stroke = shapeTool.stroke;
    addVectorShapeLayer(shape, QCoreApplication::translate("Names", "Shape"));
}

bool EditorSession::addComponentToTarget(const VectorPath& path, const std::optional<LiveShape>& live, const QString& name) {
    if (!canEditLayers()) return false;
    if (vectorMaskTargeted()) {
        // Into the layer's vector mask (an empty one, Reveal All, becomes just this outline).
        Layer* layer = activeLayerMutable();
        VectorPath mask = layerVectorMask(*layer, *document_).value_or(VectorPath{});
        addShapeComponent(mask, path, pathOp.value_or(VectorPath::Op::Add));
        beginEdit(name);
        setLayerVectorMask(*layer, *document_, mask);
        endEdit();
        notifyDocument();
        emit transformChanged();
        return true;
    }
    if (!pathOp) return false;
    auto shape = activeVectorShape();
    if (!shape) return false;
    const int32_t group = addShapeComponent(shape->path, path, *pathOp);
    if (live) { LiveShape l = *live; l.group = group; shape->live.push_back(l); }
    return setActiveVectorShape(*shape, name);
}

void EditorSession::setSelectedSubpath(std::optional<int> index) {
    if (index == selectedSubpath_) return;
    selectedSubpath_ = index;
    emit toolChanged();
}

bool EditorSession::setSelectedSubpathOp(VectorPath::Op op) {
    auto path = targetPath();
    if (!path || !selectedSubpath_ || *selectedSubpath_ < 0 || *selectedSubpath_ >= int(path->subpaths.size())) return false;
    if (path->subpaths[size_t(*selectedSubpath_)].op == op) return true;
    setComponentOp(*path, *selectedSubpath_, op);
    return setTargetPath(*path, QT_TRANSLATE_NOOP("History", "Path Operation"));
}

bool EditorSession::mergeTargetComponents() {
    auto path = targetPath();
    if (!path || path->subpaths.empty()) return false;
    const VectorPath merged = mergeShapeComponents(*path);
    if (merged.subpaths.empty()) return false;
    selectedSubpath_.reset();
    return setTargetPath(merged, QT_TRANSLATE_NOOP("History", "Merge Shape Components"));
}

// ---- Vector masks on layers -------------------------------------------------------------------------------------

bool EditorSession::vectorMaskTargeted() const {
    const Layer* layer = activeLayer();
    return layer && document_ && vectorMaskTarget_ == activeLayerId_ && hasLayerVectorMask(*layer);
}

bool EditorSession::targetVectorMask(const Uuid& id) {
    if (!document_) return false;
    const Layer* layer = document_->find(id);
    if (!layer || !hasLayerVectorMask(*layer)) return false;
    selectLayer(id, false);
    vectorMaskTarget_ = id;
    activePathId_.reset();
    selectedSubpath_.reset();
    emit layersChanged();
    emit pathsChanged();
    emit transformChanged();
    return true;
}

bool EditorSession::addVectorMask(VectorMaskKind kind, QString* error) {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"), error)) return false;
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    if (!canEditLayers()) return fail(tr("The document is busy."));
    Layer* layer = activeLayerMutable();
    if (!layer) return fail(tr("No active layer."));
    if (layer->isGroup || layer->adjustment) return fail(tr("A vector mask goes on a layer here, not on a folder or an adjustment layer."));
    if (isVectorShapeLayer(*layer)) return fail(tr("A shape layer's path is its vector mask already."));
    if (hasLayerVectorMask(*layer)) return fail(tr("The layer has a vector mask already."));
    VectorPath path;
    if (kind == VectorMaskKind::HideAll) path.inverted = true;
    if (kind == VectorMaskKind::CurrentPath) {
        auto chosen = activePathId_ ? documentPath(*document_, *activePathId_) : std::nullopt;
        if (!chosen || chosen->path.subpaths.empty()) return fail(tr("Choose a path in the Paths panel first."));
        path = chosen->path;
    }
    beginEdit(kind == VectorMaskKind::CurrentPath ? QT_TRANSLATE_NOOP("History", "Add Vector Mask") : kind == VectorMaskKind::HideAll ? QT_TRANSLATE_NOOP("History", "Hide All Vector Mask") : QT_TRANSLATE_NOOP("History", "Reveal All Vector Mask"));
    setLayerVectorMask(*layer, *document_, path);
    endEdit();
    vectorMaskTarget_ = layer->id;
    activePathId_.reset();
    notifyDocument();
    emit pathsChanged();
    emit transformChanged();
    return true;
}

bool EditorSession::deleteVectorMask() {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"))) return false;
    if (!canEditLayers()) return false;
    Layer* layer = activeLayerMutable();
    if (!layer || !hasLayerVectorMask(*layer)) return false;
    beginEdit(QT_TRANSLATE_NOOP("History", "Delete Vector Mask"));
    setLayerVectorMask(*layer, *document_, std::nullopt);
    endEdit();
    vectorMaskTarget_.reset();
    notifyDocument();
    emit transformChanged();
    return true;
}

bool EditorSession::setVectorMaskPath(const Uuid& id, const VectorPath& path, QString* error) {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"), error)) return false;
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    if (!canEditLayers()) return fail(tr("The document is busy."));
    Layer* layer = document_->find(id);
    if (!layer) return fail(tr("No such layer."));
    if (layer->isGroup || layer->adjustment) return fail(tr("A vector mask goes on a layer here, not on a folder or an adjustment layer."));
    if (isVectorShapeLayer(*layer)) return fail(tr("A shape layer's path is its vector mask: change it with shape.set."));
    beginEdit(hasLayerVectorMask(*layer) ? QT_TRANSLATE_NOOP("History", "Edit Vector Mask") : QT_TRANSLATE_NOOP("History", "Add Vector Mask"));
    setLayerVectorMask(*layer, *document_, path);
    endEdit();
    notifyDocument();
    emit transformChanged();
    return true;
}

// ---- Text to paths ----------------------------------------------------------------------------------------------

bool EditorSession::textToWorkPath(const Uuid& id, QString* error) {
    if (refusedAtDepth("edit.text", tr("Text"), error)) return false;
    if (!canEditLayers()) { if (error) *error = tr("The document is busy."); return false; }
    const Layer* layer = document_->find(id);
    if (!layer) { if (error) *error = tr("No such layer."); return false; }
    auto path = textLayerOutline(*layer, error);
    if (!path) return false;
    return storePath(kWorkPathId, QString(), *path) != 0;
}

bool EditorSession::textToShape(const Uuid& id, QString* error) {
    if (refusedAtDepth("edit.paint", tr("Painting"), error)) return false;
    if (!canEditLayers()) { if (error) *error = tr("The document is busy."); return false; }
    const Layer* text = document_->find(id);
    if (!text) { if (error) *error = tr("No such layer."); return false; }
    auto path = textLayerOutline(*text, error);
    if (!path) return false;
    // The text layer becomes a shape layer in its place: its name, look and style stay, its type does not.
    Layer layer = *text;
    if (layer.psdCarry) {
        auto carry = std::make_shared<PsdLayerCarry>(*layer.psdCarry);
        carry->blocks.erase(std::remove_if(carry->blocks.begin(), carry->blocks.end(), [](const PsdBlock& b) { return b.key == "TySh" || b.key == "tySh"; }), carry->blocks.end());
        layer.psdCarry = carry;
    }
    layer.extraJson.clear();
    VectorShape shape;
    shape.path = *path;
    auto byte = [](double v) { return uint8_t(std::clamp(std::lround(v * 255), 0L, 255L)); };
    shape.r = byte(layer.text->red); shape.g = byte(layer.text->green); shape.b = byte(layer.text->blue);
    if (textInkMatches(layer.text->red, layer.text->green, layer.text->blue, layer.text->ink)) shape.ink = layer.text->ink;   // a CMYK file's inks
    shape.fill = true;
    shape.stroke.enabled = false;
    setVectorShape(layer, *document_, shape);
    const int index = document_->indexOf(id);
    if (index < 0) return false;
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Convert to Shape"));
    document_->layers[size_t(index)] = layer;
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
    return true;
}

// ---- Live shape properties ------------------------------------------------------------------------------------

std::vector<LiveShape> EditorSession::activeLiveShapes() const {
    auto shape = activeVectorShape();
    return shape ? shape->live : std::vector<LiveShape>{};
}

bool EditorSession::setActiveLiveShape(const LiveShape& live) {
    auto shape = activeVectorShape();
    if (!shape) return false;
    auto entry = std::find_if(shape->live.begin(), shape->live.end(), [&](const LiveShape& l) { return l.group == live.group; });
    if (entry == shape->live.end() || !(live.box.width > 0) || !(live.box.height > 0)) return false;
    // The group's subpath redrawn from the new properties, where it was and with its operation.
    auto at = std::find_if(shape->path.subpaths.begin(), shape->path.subpaths.end(), [&](const VectorPath::Subpath& s) { return s.group == live.group; });
    if (at == shape->path.subpaths.end()) return false;
    VectorPath::Subpath made = livePath(live).subpaths[0];
    made.op = at->op;
    *at = made;
    *entry = live;
    return setActiveVectorShape(*shape, QT_TRANSLATE_NOOP("History", "Live Shape Properties"));
}

// ---- The target path --------------------------------------------------------------------------------------------

std::vector<DocumentPath> EditorSession::paths() const { return document_ ? documentPaths(*document_) : std::vector<DocumentPath>{}; }

void EditorSession::selectPath(std::optional<uint16_t> id) {
    if (id && (!document_ || !documentPath(*document_, *id))) id.reset();
    if (id == activePathId_) return;
    activePathId_ = id;
    selectedSubpath_.reset();
    if (id) vectorMaskTarget_.reset();
    emit pathsChanged();
    emit transformChanged();
}

std::optional<VectorPath> EditorSession::targetPath() const {
    if (!document_) return std::nullopt;
    if (activePathId_) if (auto p = documentPath(*document_, *activePathId_)) return p->path;
    if (vectorMaskTargeted()) if (auto mask = layerVectorMask(*activeLayer(), *document_)) return mask;
    if (auto shape = activeVectorShape()) return shape->path;
    return std::nullopt;
}

bool EditorSession::beginPathEdit(const QString& name) {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"))) return false;
    if (pathEditing_ || !canEditLayers() || !targetPath()) return false;
    beginEdit(name);
    pathEditing_ = true;
    return true;
}

void EditorSession::updatePathEdit(const VectorPath& path) {
    if (!pathEditing_ || !document_) return;
    if (activePathId_ && documentPath(*document_, *activePathId_)) setDocumentPath(*document_, *activePathId_, "", path);
    else if (vectorMaskTargeted()) setLayerVectorMask(*activeLayerMutable(), *document_, path);
    else if (Layer* layer = activeLayerMutable(); layer && isVectorShapeLayer(*layer)) {
        auto shape = vectorShapeOf(*layer, *document_);
        if (!shape) return;
        shape->path = path;
        setVectorShape(*layer, *document_, *shape);
    }
    emit documentChanged({});
    emit transformChanged();
}

void EditorSession::endPathEdit() {
    if (!pathEditing_) return;
    pathEditing_ = false;
    endEdit();
    notifyDocument();
    emit pathsChanged();
}

bool EditorSession::setTargetPath(const VectorPath& path, const QString& name) {
    if (!beginPathEdit(name)) return false;
    updatePathEdit(path);
    endPathEdit();
    return true;
}

bool EditorSession::addAnchorAt(QPointF p, double radius) {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"))) return false;
    auto path = targetPath();
    if (!path) return false;
    auto hit = nearestPathSegment(*path, toPoint(p));
    if (!hit || hit->distance > radius) return false;
    insertAnchor(*path, hit->subpath, hit->segment, hit->t);
    return setTargetPath(*path, QT_TRANSLATE_NOOP("History", "Add Anchor Point"));
}

bool EditorSession::deleteAnchorAt(QPointF p, double radius) {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"))) return false;
    auto path = targetPath();
    if (!path) return false;
    auto knot = nearestKnot(*path, toPoint(p), radius);
    if (!knot) return false;
    removeAnchor(*path, knot->first, knot->second);
    return setTargetPath(*path, QT_TRANSLATE_NOOP("History", "Delete Anchor Point"));
}

// ---- The Paths panel's commands ---------------------------------------------------------------------------------

uint16_t EditorSession::newPath(const QString& name) {
    if (!canEditLayers()) return 0;
    beginEdit(QT_TRANSLATE_NOOP("History", "New Path"));
    const uint16_t id = setDocumentPath(*document_, 0, name.toStdString(), VectorPath{});
    endEdit();
    if (!id) { emit error(tr("A document holds at most 998 saved paths.")); return 0; }
    activePathId_ = id;
    notifyDocument();
    emit pathsChanged();
    return id;
}

uint16_t EditorSession::storePath(uint16_t id, const QString& name, const VectorPath& path) {
    if (!canEditLayers()) return 0;
    beginEdit(id == 0 ? QT_TRANSLATE_NOOP("History", "New Path") : id == kWorkPathId ? QT_TRANSLATE_NOOP("History", "Work Path") : QT_TRANSLATE_NOOP("History", "Edit Path"));
    id = setDocumentPath(*document_, id, name.toStdString(), path);
    endEdit();
    if (!id) { emit error(tr("A document holds at most 998 saved paths.")); return 0; }
    activePathId_ = id;
    notifyDocument();
    emit pathsChanged();
    emit transformChanged();
    return id;
}

void EditorSession::renamePath(uint16_t id, const QString& name) {
    if (!canEditLayers() || id == kWorkPathId || name.trimmed().isEmpty()) return;
    beginEdit(QT_TRANSLATE_NOOP("History", "Rename Path"));
    renameDocumentPath(*document_, id, name.trimmed().toStdString());
    endEdit();
    notifyDocument();
    emit pathsChanged();
}

void EditorSession::deletePath(uint16_t id) {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"))) return;
    if (!canEditLayers()) return;
    beginEdit(QT_TRANSLATE_NOOP("History", "Delete Path"));
    removeDocumentPath(*document_, id);
    endEdit();
    if (activePathId_ == id) activePathId_.reset();
    notifyDocument();
    emit pathsChanged();
    emit transformChanged();
}

void EditorSession::savePath(uint16_t id, const QString& name) {
    if (!canEditLayers()) return;
    auto p = documentPath(*document_, id);
    if (!p) return;
    beginEdit(QT_TRANSLATE_NOOP("History", "Save Path"));
    const uint16_t saved = setDocumentPath(*document_, 0, name.trimmed().isEmpty() ? QCoreApplication::translate("Names", "Path").toStdString() : name.trimmed().toStdString(), p->path);
    if (saved && id == kWorkPathId) removeDocumentPath(*document_, kWorkPathId);   // no room: the Work Path stays
    endEdit();
    if (!saved) { emit error(tr("A document holds at most 998 saved paths.")); return; }
    activePathId_ = saved;
    notifyDocument();
    emit pathsChanged();
}

bool EditorSession::pathToSelection(uint16_t id, SelectionMode mode) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return false;
    if (!document_) return false;
    auto p = documentPath(*document_, id);
    if (!p || p->path.subpaths.empty()) return false;
    auto coverage = rasterizeVectorMask(p->path, document_->rect(), 1, document_->width, document_->height);
    applySelectionShape(*coverage, mode, QT_TRANSLATE_NOOP("History", "Make Selection"));
    return true;
}

bool EditorSession::fillPath(uint16_t id) {
    if (refusedAtDepth("edit.paint", tr("Painting"))) return false;
    if (!canEditLayers()) return false;
    auto p = documentPath(*document_, id);
    if (!p || p->path.subpaths.empty()) return false;
    const Layer* target = activeLayer();
    if (document_->colorMode != ColorMode::RGB && !(isMaskSelected_ && target && target->mask)) {
        // CMYK and Lab: the foreground colour through the profile, painted in the document's channels (a layer mask
        // takes the grey, below, as in RGB).
        const AnyGray coverage = document_->sampleType == SampleType::U16 ? AnyGray(rasterizeVectorMask16(p->path, document_->rect(), 1, document_->width, document_->height))
                                                                         : AnyGray(rasterizeVectorMask(p->path, document_->rect(), 1, document_->width, document_->height));
        return fillThroughMode(foregroundColor, QT_TRANSLATE_NOOP("History", "Fill Path"), &coverage, brushSettings.opacity);
    }
    if (document_->sampleType == SampleType::U16) {
        auto coverage = rasterizeVectorMask16(p->path, document_->rect(), 1, document_->width, document_->height);
        return fillThrough16(foregroundColor, faded(*coverage, brushSettings.opacity).get(), QT_TRANSLATE_NOOP("History", "Fill Path"));
    }
    auto coverage = rasterizeVectorMask(p->path, document_->rect(), 1, document_->width, document_->height);
    return fillThrough(foregroundColor, coverage.get(), brushSettings.opacity, QT_TRANSLATE_NOOP("History", "Fill Path"));
}

bool EditorSession::strokePath(uint16_t id) {
    if (refusedAtDepth("edit.paint", tr("Painting"))) return false;
    // With the brush's size and opacity in the foreground colour (Photoshop strokes with the chosen tool's tip).
    if (!canEditLayers()) return false;
    auto p = documentPath(*document_, id);
    if (!p || p->path.subpaths.empty()) return false;
    VectorStroke stroke;
    stroke.enabled = true;
    stroke.width = std::max(1.0, brushSettings.diameter);
    stroke.cap = VectorStroke::Cap::Round;
    stroke.join = VectorStroke::Join::Round;
    const Layer* target = activeLayer();
    if (document_->colorMode != ColorMode::RGB && !(isMaskSelected_ && target && target->mask)) {
        const AnyGray band = document_->sampleType == SampleType::U16 ? AnyGray(rasterizeVectorStroke16(p->path, stroke, document_->rect(), 1, document_->width, document_->height))
                                                                     : AnyGray(rasterizeVectorStroke(p->path, stroke, document_->rect(), 1, document_->width, document_->height));
        return fillThroughMode(foregroundColor, QT_TRANSLATE_NOOP("History", "Stroke Path"), &band, brushSettings.opacity);
    }
    if (document_->sampleType == SampleType::U16) {
        auto band = rasterizeVectorStroke16(p->path, stroke, document_->rect(), 1, document_->width, document_->height);
        return fillThrough16(foregroundColor, faded(*band, brushSettings.opacity).get(), QT_TRANSLATE_NOOP("History", "Stroke Path"));
    }
    auto band = rasterizeVectorStroke(p->path, stroke, document_->rect(), 1, document_->width, document_->height);
    return fillThrough(foregroundColor, band.get(), brushSettings.opacity, QT_TRANSLATE_NOOP("History", "Stroke Path"));
}

bool EditorSession::pathToShapeLayer(uint16_t id) {
    if (refusedAtDepth("edit.paint", tr("Painting"))) return false;
    if (!document_) return false;
    auto p = documentPath(*document_, id);
    if (!p || p->path.subpaths.empty()) return false;
    VectorShape shape;
    shape.path = p->path;
    for (auto& s : shape.path.subpaths) s.closed = true;
    shape.r = uint8_t(foregroundColor.red()); shape.g = uint8_t(foregroundColor.green()); shape.b = uint8_t(foregroundColor.blue());
    shape.fill = shapeTool.fill || !shapeTool.stroke.enabled;
    shape.fillPaint = shapeTool.fillPaint;
    shape.stroke = shapeTool.stroke;
    return addVectorShapeLayer(shape, QString::fromStdString(p->name));
}

bool EditorSession::selectionToWorkPath(double tolerance) {
    if (refusedAtDepth("edit.vector", tr("Vector masks and paths"))) return false;
    if (!canEditLayers() || !document_->selection || !document_->selection->coverage) return false;
    bool tooDetailed = false;
    // The outline is traced at half coverage, on the selection rounded to 8 bits at either depth.
    const auto coverage = coverage8(*document_->selection);
    if (!coverage) return false;
    const auto loops = selectionOutline(*coverage, &tooDetailed);
    if (tooDetailed || loops.empty()) { if (tooDetailed) emit error(tr("The selection's outline is too detailed to make a path from.")); return false; }
    VectorPath path;
    for (const auto& loop : loops) {
        const auto points = simplifyLoop(loop, std::max(0.1, tolerance));
        if (points.size() < 3) continue;
        VectorPath::Subpath sub;
        sub.closed = true;
        sub.op = VectorPath::Op::Xor;   // holes come out as holes: the loops combine even-odd
        for (const Point& q : points) sub.knots.push_back({q.x, q.y, q.x, q.y, q.x, q.y});
        path.subpaths.push_back(std::move(sub));
    }
    if (path.subpaths.empty()) return false;
    beginEdit(QT_TRANSLATE_NOOP("History", "Make Work Path"));
    setDocumentPath(*document_, kWorkPathId, "", path);
    endEdit();
    activePathId_ = kWorkPathId;
    notifyDocument();
    emit pathsChanged();
    emit transformChanged();
    return true;
}

} // namespace app
