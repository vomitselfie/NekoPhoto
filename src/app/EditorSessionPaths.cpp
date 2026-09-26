// Paths: the Pen and Direct Selection tools and the Paths panel's commands (compositor/vectorlayer.h). The target
// path is the Paths panel's choice, or else the active vector shape layer's path.
#include "EditorSession.h"
#include "QtGeometry.h"
#include "compositor/selection.h"

using namespace compositor;

namespace app {

// (The outline simplification is core's simplifyLoop, vectorlayer.h.)

// ---- Pen --------------------------------------------------------------------------------------------------------

void EditorSession::penPress(QPointF p) {
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
    if (penMode == PenMode::Path) {
        // The chosen path, if it is still there (undo may have taken it away); else the Work Path.
        const bool chosen = activePathId_ && documentPath(*document_, *activePathId_);
        const uint16_t id = chosen ? *activePathId_ : kWorkPathId;
        VectorPath path;
        if (auto existing = documentPath(*document_, id)) path = existing->path;
        path.subpaths.push_back(sub);
        beginEdit(chosen ? "Add Subpath" : "Work Path");
        setDocumentPath(*document_, id, "", path);
        endEdit();
        activePathId_ = id;
        notifyDocument();
        emit pathsChanged();
        return;
    }
    if (penAddsToShape) if (auto shape = activeVectorShape()) {
        shape->path.subpaths.push_back(sub);
        setActiveVectorShape(*shape, tr("Add Subpath"));
        return;
    }
    VectorShape shape;
    shape.path.subpaths.push_back(sub);
    shape.r = uint8_t(foregroundColor.red()); shape.g = uint8_t(foregroundColor.green()); shape.b = uint8_t(foregroundColor.blue());
    shape.fill = shapeTool.fill || !shapeTool.stroke.enabled;
    shape.stroke = shapeTool.stroke;
    addVectorShapeLayer(shape, QStringLiteral("Shape"));
}

// ---- The target path --------------------------------------------------------------------------------------------

std::vector<DocumentPath> EditorSession::paths() const { return document_ ? documentPaths(*document_) : std::vector<DocumentPath>{}; }

void EditorSession::selectPath(std::optional<uint16_t> id) {
    if (id && (!document_ || !documentPath(*document_, *id))) id.reset();
    if (id == activePathId_) return;
    activePathId_ = id;
    emit pathsChanged();
    emit transformChanged();
}

std::optional<VectorPath> EditorSession::targetPath() const {
    if (!document_) return std::nullopt;
    if (activePathId_) if (auto p = documentPath(*document_, *activePathId_)) return p->path;
    if (auto shape = activeVectorShape()) return shape->path;
    return std::nullopt;
}

bool EditorSession::beginPathEdit(const QString& name) {
    if (pathEditing_ || !canEditLayers() || !targetPath()) return false;
    beginEdit(name);
    pathEditing_ = true;
    return true;
}

void EditorSession::updatePathEdit(const VectorPath& path) {
    if (!pathEditing_ || !document_) return;
    if (activePathId_ && documentPath(*document_, *activePathId_)) setDocumentPath(*document_, *activePathId_, "", path);
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
    auto path = targetPath();
    if (!path) return false;
    auto hit = nearestPathSegment(*path, toPoint(p));
    if (!hit || hit->distance > radius) return false;
    insertAnchor(*path, hit->subpath, hit->segment, hit->t);
    return setTargetPath(*path, tr("Add Anchor Point"));
}

bool EditorSession::deleteAnchorAt(QPointF p, double radius) {
    auto path = targetPath();
    if (!path) return false;
    auto knot = nearestKnot(*path, toPoint(p), radius);
    if (!knot) return false;
    removeAnchor(*path, knot->first, knot->second);
    return setTargetPath(*path, tr("Delete Anchor Point"));
}

// ---- The Paths panel's commands ---------------------------------------------------------------------------------

uint16_t EditorSession::newPath(const QString& name) {
    if (!canEditLayers()) return 0;
    beginEdit("New Path");
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
    beginEdit(id == 0 ? "New Path" : id == kWorkPathId ? "Work Path" : "Edit Path");
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
    beginEdit("Rename Path");
    renameDocumentPath(*document_, id, name.trimmed().toStdString());
    endEdit();
    notifyDocument();
    emit pathsChanged();
}

void EditorSession::deletePath(uint16_t id) {
    if (!canEditLayers()) return;
    beginEdit("Delete Path");
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
    beginEdit("Save Path");
    const uint16_t saved = setDocumentPath(*document_, 0, name.trimmed().isEmpty() ? std::string("Path") : name.trimmed().toStdString(), p->path);
    if (saved && id == kWorkPathId) removeDocumentPath(*document_, kWorkPathId);   // no room: the Work Path stays
    endEdit();
    if (!saved) { emit error(tr("A document holds at most 998 saved paths.")); return; }
    activePathId_ = saved;
    notifyDocument();
    emit pathsChanged();
}

bool EditorSession::pathToSelection(uint16_t id, SelectionMode mode) {
    if (!document_) return false;
    auto p = documentPath(*document_, id);
    if (!p || p->path.subpaths.empty()) return false;
    auto coverage = rasterizeVectorMask(p->path, document_->rect(), 1, document_->width, document_->height);
    applySelectionShape(*coverage, mode, "Make Selection");
    return true;
}

bool EditorSession::fillPath(uint16_t id) {
    if (!canEditLayers()) return false;
    auto p = documentPath(*document_, id);
    if (!p || p->path.subpaths.empty()) return false;
    auto coverage = rasterizeVectorMask(p->path, document_->rect(), 1, document_->width, document_->height);
    return fillThrough(foregroundColor, coverage.get(), brushSettings.opacity, "Fill Path");
}

bool EditorSession::strokePath(uint16_t id) {
    // With the brush's size and opacity in the foreground colour (Photoshop strokes with the chosen tool's tip).
    if (!canEditLayers()) return false;
    auto p = documentPath(*document_, id);
    if (!p || p->path.subpaths.empty()) return false;
    VectorStroke stroke;
    stroke.enabled = true;
    stroke.width = std::max(1.0, brushSettings.diameter);
    stroke.cap = VectorStroke::Cap::Round;
    stroke.join = VectorStroke::Join::Round;
    auto band = rasterizeVectorStroke(p->path, stroke, document_->rect(), 1, document_->width, document_->height);
    return fillThrough(foregroundColor, band.get(), brushSettings.opacity, "Stroke Path");
}

bool EditorSession::pathToShapeLayer(uint16_t id) {
    if (!document_) return false;
    auto p = documentPath(*document_, id);
    if (!p || p->path.subpaths.empty()) return false;
    VectorShape shape;
    shape.path = p->path;
    for (auto& s : shape.path.subpaths) s.closed = true;
    shape.r = uint8_t(foregroundColor.red()); shape.g = uint8_t(foregroundColor.green()); shape.b = uint8_t(foregroundColor.blue());
    shape.fill = shapeTool.fill || !shapeTool.stroke.enabled;
    shape.stroke = shapeTool.stroke;
    return addVectorShapeLayer(shape, QString::fromStdString(p->name));
}

bool EditorSession::selectionToWorkPath(double tolerance) {
    if (!canEditLayers() || !document_->selection || !document_->selection->coverage) return false;
    bool tooDetailed = false;
    const auto loops = selectionOutline(*document_->selection->coverage, &tooDetailed);
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
    beginEdit("Make Work Path");
    setDocumentPath(*document_, kWorkPathId, "", path);
    endEdit();
    activePathId_ = kWorkPathId;
    notifyDocument();
    emit pathsChanged();
    emit transformChanged();
    return true;
}

} // namespace app
