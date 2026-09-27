// EditorSession: The session: construction, the document, history, crop and canvas, tools and the view.
#include "compositor/vectorlayer.h"
#include "EditorSession.h"
#include "compositor/depth.h"
#include "compositor/supports.h"
#include "compositor/smartfilter.h"
#include "QtGeometry.h"
#include "compositor/project.h"
#include <QFileInfo>
#include <algorithm>
#include <set>

using namespace compositor;

namespace app {

EditorSession::~EditorSession() {
    if (quickSelectThread_.joinable()) quickSelectThread_.join();
}

EditorSession::EditorSession(QObject* parent) : QObject(parent) {
    // Spot healing previews its result once the pointer has paused; the stroke goes on from there.
    healPreview_.setSingleShot(true);
    healPreview_.setInterval(180);
    connect(&healPreview_, &QTimer::timeout, this, [this] {
        if (!stroke_ || strokeMask_) return;
        stroke_->previewHeal();
        emit documentChanged({});
    });
    // Presets with slow position tracking trail the pointer and only move on input; a mouse held still sends
    // none, so the brush would stop short of it. The last input repeats at 60 a second until it has caught up.
    myPaintSettle_.setInterval(16);
    connect(&myPaintSettle_, &QTimer::timeout, this, [this] {
        if (!myPaint_ || !stroke_ || !lastBrushPoint_ || myPaint_->settled()) { myPaintSettle_.stop(); return; }
        myPaintTo(*lastBrushPoint_);
        Rect dirty = stroke_->takeDirtyRect();
        if (!dirty.isEmpty()) {
            strokeRegion_ = strokeRegion_.isEmpty() ? toQRect(dirty) : strokeRegion_.united(toQRect(dirty));
            emit documentChanged(toQRect(dirty));
        }
    });
}

QString EditorSession::title() const {
    if (!document_) return QStringLiteral("NekoPhoto");
    QString name = !projectPath_.isEmpty() ? QFileInfo(projectPath_).completeBaseName() : !importedName_.isEmpty() ? importedName_ : tr("Untitled");
    return name + (isModified() ? QStringLiteral(" *") : QString());
}

void EditorSession::notifyDocument(QRectF region) {
    documentRevision_++;
    emit documentChanged(region);
    emit layersChanged();
    emit historyChanged();
    emit titleChanged();
}

const Layer* EditorSession::activeLayer() const {
    if (!document_ || !activeLayerId_) return nullptr;
    return document_->find(*activeLayerId_);
}

Layer* EditorSession::activeLayerMutable() {
    if (!document_ || !activeLayerId_) return nullptr;
    return document_->find(*activeLayerId_);
}

void EditorSession::setActiveLayer(const std::optional<Uuid>& id) {
    if (id != activeLayerId_) isMaskSelected_ = false;
    activeLayerId_ = id;
    selectedLayerIds_.clear();
    if (id) selectedLayerIds_.insert(*id);
}

bool EditorSession::canEditLayers() const {
    // An open warp cage, a live Layer Style edit or a path drag holds an undo step open: nothing else may land in it.
    return document_ && !stroke_ && !warp_ && !transformEdit_ && !pixelMove_ && !warpCage_ && !styleEditLayer_ && !pathEditing_;
}

// ---- Document ----------------------------------------------------------------

void EditorSession::createDocument(int width, int height, double resolution, bool emptyLayer) {
    if (const BudgetCheck check = Document::canCreate(width, height, SampleType::U8); !check) { emit error(budgetText(check)); return; }
    commitTransform();
    beginEdit(QT_TRANSLATE_NOOP("History", "New Canvas"));
    Document doc(width, height);
    doc.resolution = resolution;
    std::optional<Uuid> active;
    if (emptyLayer) { doc.layers.emplace_back(QCoreApplication::translate("Names", "Layer 1").toStdString(), doc.size()); active = doc.layers.back().id; }
    document_ = doc;
    setActiveLayer(active);
    projectPath_.clear();
    history_.reset();
    endEdit();
    history_.reset();
    viewport.fit({double(width), double(height)});
    emit viewportChanged();
    emit projectPathChanged();
    notifyDocument();
    emit selectionChanged();
}

std::optional<EditorSession::LoadedProject> EditorSession::readProject(const QString& path, QString* error) {
    ProjectError err;
    auto doc = loadProject(path.toStdString(), err);
    if (!doc) { if (error) *error = QString::fromStdString(err.message); return std::nullopt; }
    std::optional<Uuid> active = loadedActiveLayer(path.toStdString());
    if (active && !doc->find(*active)) active.reset();
    return LoadedProject{std::move(*doc), active, path};
}

bool EditorSession::openProject(const QString& path, QString* error) {
    auto project = readProject(path, error);
    if (!project) return false;
    installProject(std::move(*project));
    return true;
}

void EditorSession::installProject(LoadedProject project) {
    commitTransform();
    previewBase_.reset();   // playback was over the document this replaces
    document_ = std::move(project.document);
    setActiveLayer(project.activeLayer);
    projectPath_ = project.path;
    history_.reset();
    watchProject();
    viewport.fit({double(document_->width), double(document_->height)});
    emit viewportChanged();
    emit projectPathChanged();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::adoptDocument(const Document& document, const QString& name) {
    commitTransform();
    previewBase_.reset();
    document_ = document;
    // The topmost visible pixel layer starts active (a hidden top layer, common in exports, would confuse).
    std::optional<Uuid> active;
    std::set<Uuid> visible = effectiveVisibleIds(document_->layers);
    for (auto it = document_->layers.rbegin(); it != document_->layers.rend(); ++it) if (!it->isGroup && visible.count(it->id)) { active = it->id; break; }
    if (!active) for (auto it = document_->layers.rbegin(); it != document_->layers.rend(); ++it) if (!it->isGroup) { active = it->id; break; }
    setActiveLayer(active);
    projectPath_.clear();
    stopWatchingProject();
    importedName_ = name;
    history_.reset();
    viewport.fit({double(document_->width), double(document_->height)});
    emit viewportChanged();
    emit projectPathChanged();
    emit titleChanged();
    notifyDocument();
    emit selectionChanged();
}

bool EditorSession::saveProject(const QString& path, QString* error) {
    endTemporaryLayers();   // the Quick Mask and filter-mask layers are never saved
    if (!document_) return false;
    commitTransform();
    ProjectError err;
    // The document itself, never a frame playback shows (endTemporaryLayers stops playback; this holds regardless).
    if (!compositor::saveProject(documentToSave(), activeLayerId_, path.toStdString(), err)) {
        if (error) *error = QString::fromStdString(err.message);
        return false;
    }
    projectPath_ = path;
    history_.markSaved();
    watchProject();   // our own save is the package as we know it
    emit projectPathChanged();
    emit titleChanged();
    emit historyChanged();
    return true;
}

void EditorSession::closeDocument() {
    cancelBrush();
    cancelTransform();
    previewBase_.reset();
    document_.reset();
    setActiveLayer(std::nullopt);
    projectPath_.clear();
    stopWatchingProject();
    history_.reset();
    emit projectPathChanged();
    notifyDocument();
    emit selectionChanged();
}

QString EditorSession::budgetText(const BudgetCheck& check) {
    const QString depth = QString::fromLatin1(sampleTypeName(check.type));
    const QString megapixels = QString::number(check.limit / 1000000);
    switch (check.kind) {
    case BudgetCheck::Ok: return {};
    case BudgetCheck::Side: return tr("An image, layer or canvas can be at most %1 pixels a side.").arg(check.limit);
    case BudgetCheck::Image: return tr("An image, layer or canvas holds up to %1 megapixels at %2 bits per channel.").arg(megapixels, depth);
    case BudgetCheck::Project: return tr("This would take the %1-bit document past its %2 megapixels for all layers together.").arg(depth, megapixels);
    case BudgetCheck::Masks: return tr("This would take the %1-bit document past its %2 megapixels for all masks together.").arg(depth, megapixels);
    case BudgetCheck::Layers: return tr("A document holds up to %1 layers.").arg(check.limit);
    }
    return {};
}

bool EditorSession::insertImage(const AnyImage& image, const QString& name, std::optional<QPointF> at, QString* errorText) {
    if (!image || image.width() <= 0 || image.height() <= 0) return false;
    const BudgetCheck check = document_ ? document_->canInsertImage(image.width(), image.height()) : Document::canCreate(image.width(), image.height(), image.sampleType());
    if (!check) {
        if (errorText) *errorText = budgetText(check);
        else emit error(budgetText(check));
        return false;
    }
    cancelBrush();
    commitTransform();
    beginEdit(QT_TRANSLATE_NOOP("History", "Import Image"));
    if (!document_) {
        document_ = Document(image.width(), image.height());
        document_->sampleType = image.sampleType();
        viewport.fit({double(image.width()), double(image.height())});
        emit viewportChanged();
        at.reset();
    }
    Point center = at ? toPoint(*at) : Point(document_->width / 2.0, document_->height / 2.0);
    Layer layer(Asset::makeAny(image, name.toStdString()), Point(std::floor(center.x - image.width() / 2.0), std::floor(center.y - image.height() / 2.0)));
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    if (layer.parentId) collapsedGroupIds.erase(*layer.parentId);
    // Above the active layer, as on the Mac.
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    if (active && active->isGroup) {
        auto inside = descendantIds(document_->layers, active->id);
        for (size_t i = 0; i < document_->layers.size(); i++) if (inside.count(document_->layers[i].id)) index = std::max(index, int(i) + 1);
    }
    document_->layers.insert(document_->layers.begin() + std::min(index, int(document_->layers.size())), layer);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
    return true;
}

std::shared_ptr<Image> EditorSession::flattened() const {
    if (!document_) return nullptr;
    if (document_->sampleType == SampleType::U16) return ditherToEightBit(*renderFlattened16(*document_));
    return renderFlattened(*document_);
}

std::shared_ptr<Image16> EditorSession::flattened16() const {
    if (!document_) return nullptr;
    return renderFlattened16(*document_);
}

// ---- Bit depth ---------------------------------------------------------------

bool EditorSession::supportsFeature(std::string_view feature) const { return supports(feature, sampleType()); }

bool EditorSession::refusedAtDepth(std::string_view feature, const QString& what, QString* errorText) {
    if (supportsFeature(feature)) return false;
    const QString message = tr("%1 is not available for %2-bit documents yet.").arg(what, QString::fromLatin1(sampleTypeName(sampleType())));
    if (errorText) *errorText = message;
    else emit error(message);
    return true;
}

std::shared_ptr<const GrayImage> EditorSession::selectionCoverage8() const {
    if (!document_ || !document_->selection) return nullptr;
    return coverage8(*document_->selection);
}

std::shared_ptr<const GrayImage> EditorSession::coverage8(const Selection& selection) {
    const AnyGray& coverage = selection.coverage;
    if (!coverage || coverage.u8()) return coverage.u8();
    // One reduction kept, for the selection on screen (the canvas asks on every tick of its outline).
    static std::weak_ptr<const Gray16> held;
    static std::shared_ptr<const GrayImage> reduced;
    if (held.lock() != coverage.u16()) { held = coverage.u16(); reduced = coverage.u16() ? narrowGray(*coverage.u16()) : nullptr; }
    return reduced;
}

bool EditorSession::convertMode(SampleType type, QString* errorText) {
    if (!document_ || type == document_->sampleType) return document_.has_value();
    if (type == SampleType::F32) { if (errorText) *errorText = tr("32-bit documents are not available yet."); return false; }
    commitTransform();
    const std::string problem = sampleTypeBudgetProblem(*document_, type);
    if (!problem.empty()) {
        if (errorText) *errorText = tr("This document is too large for %1 bits per channel: %2").arg(QString::fromLatin1(sampleTypeName(type)), QString::fromStdString(problem));
        return false;
    }
    // Leaving a tool mid-way first: a stroke or a floating edit holds pixels at the old depth.
    cancelBrush();
    beginEdit(QT_TRANSLATE_NOOP("History", "Convert Mode"));
    std::string why;
    const bool ok = convertSampleType(*document_, type, &why);
    endEdit();
    if (!ok) { if (errorText) *errorText = QString::fromStdString(why); return false; }
    // A tool that does not work at the new depth gives way to the Move tool.
    if (!toolSupportedAtDepth(tool_)) selectTool(Tool::Move);
    notifyDocument();
    emit selectionChanged();
    emit toolChanged();
    return true;
}

// ---- History -----------------------------------------------------------------

bool EditorSession::canUndo() const { return document_ && !stroke_ && !warp_ && !pixelMove_ && !transformEdit_ && (history_.canUndo() || gradient_); }
bool EditorSession::canRedo() const { return document_ && !stroke_ && !warp_ && !pixelMove_ && !transformEdit_ && !gradient_ && history_.canRedo(); }

void EditorSession::undo() {
    // Like Photoshop, the first Undo discards a pending gradient.
    if (gradient_) { cancelGradient(); return; }
    if (warpCage_) { cancelWarpCage(); return; }   // likewise an open warp cage
    if (!canUndo()) return;
    auto snapshot = history_.undo();
    if (snapshot) restore(*snapshot);
}

void EditorSession::redo() {
    if (warpCage_) cancelWarpCage();
    if (!canRedo()) return;
    auto snapshot = history_.redo();
    if (snapshot) restore(*snapshot);
}

void EditorSession::restore(const DocumentHistory::Snapshot& snapshot) {
    bool changedCanvas = !document_ || !snapshot.document || document_->id != snapshot.document->id || document_->width != snapshot.document->width || document_->height != snapshot.document->height;
    bool keepMask = isMaskSelected_ && activeLayerId_ == snapshot.activeLayerId;
    // Only what the step changed is rendered again: undoing a brush stroke redraws the stroke's layer, not the view.
    previewBase_.reset();   // the snapshot is the document as it was; playback's states belong to the one it replaces
    const Rect changed = changedCanvas ? Rect() : !history_.stepRegion().isEmpty() ? history_.stepRegion() : changedArea(*document_, *snapshot.document);
    document_ = snapshot.document;
    setActiveLayer(snapshot.activeLayerId);
    const Layer* active = activeLayer();
    isMaskSelected_ = keepMask && active && active->mask;
    if (active && active->mask && filterMaskLayer_ == activeLayerId_) isMaskSelected_ = true;   // the filter mask's layer paints its mask alone
    if (changedCanvas && document_) { viewport.fit({double(document_->width), double(document_->height)}); emit viewportChanged(); }
    if (changedCanvas) notifyDocument();
    else if (changed.isEmpty()) {
        // Nothing visible changed (a rename, a selection, a lock): the canvas keeps what it shows.
        documentRevision_++;
        emit documentChangedAsShown();
        emit layersChanged();
        emit historyChanged();
        emit titleChanged();
    } else notifyDocument(toQRect(changed));
    emit selectionChanged();
}

int EditorSession::squashHistory(uint64_t since, const QString& name) {
    const int merged = history_.squash(since, name.toStdString());
    if (merged > 1) emit historyChanged();
    return merged;
}

void EditorSession::beginEdit(const QString& name) { endFramePreview(); history_.begin(name.toStdString(), document_, activeLayerId_); }
void EditorSession::endEdit() {
    // Warped and filtered smart objects moved or scaled in this edit are drawn again from their contents.
    syncFilterMask();   // a painted filter mask goes into its Smart Filters in the same step
    if (document_) { refreshSmartObjectRasters(*document_); refreshVectorShapes(*document_); }
    // The frame the layers show keeps what this edit did to their visibility, position and opacity.
    if (document_) { pruneAnimation(*document_); syncCurrentFrame(*document_); }
    // One depth per document: pixels this edit brought in at another depth (an 8-bit file imported into a 16-bit
    // document, a raster an 8-bit path drew) are converted to the document's.
    if (document_) conformToSampleType(*document_);
    history_.end(document_, activeLayerId_);
}


// ---- Crop and canvas --------------------------------------------------------------

bool EditorSession::trim(const TrimOptions& options) {
    if (refusedAtDepth("edit.crop", tr("Cropping"))) return false;
    if (!canEditLayers()) return false;
    auto flat = renderFlattened(*document_);
    auto rect = flat ? trimRect(*flat, options) : std::nullopt;
    if (!rect || *rect == document_->rect()) return false;
    cropTo(QRectF(rect->x, rect->y, rect->width, rect->height), QT_TRANSLATE_NOOP("History", "Trim"));
    return true;
}

void EditorSession::cropTo(const QRectF& rectF, const char* action) {
    if (refusedAtDepth("edit.crop", tr("Cropping"))) return;
    if (!canEditLayers()) return;
    Rect rect = Rect(rectF.x(), rectF.y(), rectF.width(), rectF.height()).integral().intersection(document_->rect());
    if (rect.isEmpty() || rect == document_->rect()) return;
    beginEdit(action);
    Document doc = *document_;
    doc.width = int(rect.width);
    doc.height = int(rect.height);
    for (auto& l : doc.layers) {
        l.transform.origin.x -= rect.x;
        l.transform.origin.y -= rect.y;
        if (l.mask && l.mask->placement) { l.mask->placement->origin.x -= rect.x; l.mask->placement->origin.y -= rect.y; }
    }
    offsetAnimation(doc, -rect.x, -rect.y);
    if (doc.selection && doc.selection->coverage.u8()) doc.selection->coverage = cropGray(*doc.selection->coverage.u8(), int(rect.x), int(rect.y), doc.width, doc.height);
    else if (doc.selection && doc.selection->coverage.u16()) doc.selection->coverage = Gray16Ptr(cropGray(*doc.selection->coverage.u16(), int(rect.x), int(rect.y), doc.width, doc.height));
    document_ = doc;
    endEdit();
    viewport.fit({double(doc.width), double(doc.height)});
    emit viewportChanged();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::resizeCanvas(int width, int height, double anchorX, double anchorY) {
    if (!canEditLayers() || !Document::validDimension(width) || !Document::validDimension(height)) return;
    if (width == document_->width && height == document_->height) return;
    // The canvas budget in bytes: a 16-bit canvas holds half the pixels of an 8-bit one.
    if (const BudgetCheck check = Document::canCreate(width, height, document_->sampleType); !check) { emit error(budgetText(check)); return; }
    double dx = std::round((width - document_->width) * anchorX), dy = std::round((height - document_->height) * anchorY);
    beginEdit(QT_TRANSLATE_NOOP("History", "Canvas Size"));
    Document doc = *document_;
    doc.width = width;
    doc.height = height;
    for (auto& l : doc.layers) {
        l.transform.origin.x += dx;
        l.transform.origin.y += dy;
        if (l.mask && l.mask->placement) { l.mask->placement->origin.x += dx; l.mask->placement->origin.y += dy; }
    }
    offsetAnimation(doc, dx, dy);
    doc.selection.reset();
    document_ = doc;
    endEdit();
    viewport.fit({double(width), double(height)});
    emit viewportChanged();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::resizeImage(int width, int height, double resolution, int sampling) {
    if (refusedAtDepth("edit.imageSize", tr("Image Size"))) return;
    if (!canEditLayers()) return;
    if (const BudgetCheck check = Document::canCreate(width, height, document_->sampleType); !check) { emit error(budgetText(check)); return; }
    Document doc = *document_;
    Sampling mode = sampling == 0 ? Sampling::Nearest : sampling == 1 ? Sampling::Smooth : Sampling::High;
    const double sx = double(width) / doc.width, sy = double(height) / doc.height;
    if (!resizeDocument(doc, width, height, resolution, mode)) { emit error(tr("The resized layers would not fit the document's budgets.")); return; }
    scaleAnimation(doc, sx, sy);
    beginEdit(QT_TRANSLATE_NOOP("History", "Image Size"));
    document_ = doc;
    endEdit();
    viewport.fit({double(width), double(height)});
    emit viewportChanged();
    notifyDocument();
    emit selectionChanged();
}

// ---- Tools and view -----------------------------------------------------------------

const char* EditorSession::toolFeature(Tool tool) {
    switch (tool) {
    case Tool::Move: return "tool.move";
    case Tool::Marquee: return "tool.marquee";
    case Tool::Lasso: return "tool.lasso";
    case Tool::Wand: return "tool.wand";
    case Tool::Scribble: return "tool.quickSelect";
    case Tool::Crop: return "tool.crop";
    case Tool::Brush: return "tool.brush";
    case Tool::SpotHealing: return "tool.spotHealing";
    case Tool::CloneStamp: return "tool.cloneStamp";
    case Tool::Smudge: return "tool.smudge";
    case Tool::Gradient: return "tool.gradient";
    case Tool::Shape: return "tool.shape";
    case Tool::Eyedropper: return "tool.eyedropper";
    case Tool::Hand: return "tool.hand";
    case Tool::Zoom: return "tool.zoom";
    case Tool::Text: return "tool.text";
    case Tool::Dodge: return "tool.dodge";
    case Tool::PaintBucket: return "tool.paintBucket";
    case Tool::Pen: return "tool.pen";
    case Tool::DirectSelect: return "tool.directSelect";
    case Tool::Artboard: return "tool.artboard";
    case Tool::Slice: return "tool.slice";
    }
    return "tool.unknown";
}

void EditorSession::selectTool(Tool tool) {
    if (stroke_ || warp_ || pixelMove_) return;
    if (!toolSupportedAtDepth(tool)) { refusedAtDepth(toolFeature(tool), tr("This tool")); return; }
    if (tool != tool_) { commitTransform(); resolveGradient(); cancelShape(); cancelWarpCage(); }
    tool_ = tool;
    emit toolChanged();
}

void EditorSession::fitView() {
    if (!document_) return;
    viewport.fit({double(document_->width), double(document_->height)});
    emit viewportChanged();
}

void EditorSession::zoomTo(double zoom, std::optional<QPointF> anchor) {
    if (!document_) return;
    viewport.setZoom(zoom, anchor.value_or(viewport.center()), {double(document_->width), double(document_->height)});
    emit viewportChanged();
}

Overrides EditorSession::renderOverrides() const {
    Overrides overrides;
    if (transformEdit_ && document_) {
        const TransformEdit& edit = *transformEdit_;
        std::vector<const Layer*> targets;
        if (edit.group) { for (auto& [id, t] : edit.group->originals) if (const Layer* l = document_->find(id)) targets.push_back(l); }
        else if (const Layer* l = document_->find(edit.layerId)) targets.push_back(l);
        for (const Layer* layer : targets) {
            LayerOverride& o = overrides[layer->id];
            if (edit.mask) { o.maskPlacement = std::optional<LayerTransform>(edit.draft); continue; }
            LayerTransform shown = displayedTransform(*layer);
            o.transform = shown;
            if (layer->mask) o.maskPlacement = displayedMaskPlacement(*layer);
            if (edit.corners && layer->asset && layer->asset->image.u16()) {
                // The same at 16 bits.
                auto target = distortTarget(*layer, edit);
                if (!target) continue;
                Gray16Ptr maskImage = layer->mask && layer->mask->enabled ? layer->mask->asset.image.u16() : nullptr;
                auto it = distortCache_.find(layer->id);
                bool fresh = it != distortCache_.end() && it->second.corners == target->second && it->second.transform == target->first && it->second.source16 == layer->asset->image.u16() && it->second.mask16 == maskImage;
                if (!fresh) {
                    DistortCache cache;
                    cache.corners = target->second;
                    cache.transform = target->first;
                    cache.source16 = layer->asset->image.u16();
                    cache.mask16 = maskImage;
                    cache.image16 = warpImage(layer->asset->image.u16(), target->first, target->second, 2048);
                    if (cache.image16 && maskImage && !layer->mask->placement && layer->mask->linked) {
                        auto wm = warpMask(*maskImage, target->first, target->second, 0, 2048);
                        if (wm) cache.warpedMask16 = wm->image;
                    }
                    it = distortCache_.insert_or_assign(layer->id, std::move(cache)).first;
                }
                const DistortCache& cache = it->second;
                if (cache.image16) {
                    o.image16 = Image16Ptr(cache.image16->image);
                    o.transform = cache.image16->transform;
                    if (cache.warpedMask16) { o.maskImage16 = cache.warpedMask16; o.maskPlacement = std::optional<LayerTransform>(); }
                    else if (layer->mask) o.maskPlacement = std::optional<LayerTransform>(layer->mask->placement ? *layer->mask->placement : layer->transform);
                }
            } else if (edit.corners && layer->asset && layer->asset->image.u8()) {
                // The layer warped into the pending distortion, at preview size, cached while nothing changes.
                auto target = distortTarget(*layer, edit);
                if (!target) continue;
                GrayPtr maskImage = layer->mask && layer->mask->enabled ? layer->mask->asset.image.u8() : nullptr;
                auto it = distortCache_.find(layer->id);
                bool fresh = it != distortCache_.end() && it->second.corners == target->second && it->second.transform == target->first && it->second.source == layer->asset->image.u8() && it->second.mask == maskImage;
                if (!fresh) {
                    DistortCache cache{target->second, target->first, layer->asset->image.u8(), maskImage, warpImage(layer->asset->image.u8(), target->first, target->second, 2048), nullptr};
                    if (cache.image && maskImage && !layer->mask->placement && layer->mask->linked) {
                        auto wm = warpMask(maskImage, target->first, target->second, 0, 2048);
                        if (wm) cache.warpedMask = wm->image;
                    }
                    it = distortCache_.insert_or_assign(layer->id, std::move(cache)).first;
                }
                const DistortCache& cache = it->second;
                if (cache.image) {
                    o.image = cache.image->image;
                    o.transform = cache.image->transform;
                    if (cache.warpedMask) { o.maskImage = cache.warpedMask; o.maskPlacement = std::optional<LayerTransform>(); }
                    else if (layer->mask) o.maskPlacement = std::optional<LayerTransform>(layer->mask->placement ? *layer->mask->placement : layer->transform);
                }
            }
        }
    }
    if (blendPreview_ && activeLayerId_) overrides[*activeLayerId_].blendMode = *blendPreview_;
    if (previewImage_ && previewLayerId_ && document_ && document_->find(*previewLayerId_)) {
        LayerOverride& o = overrides[*previewLayerId_];
        if (previewImage_.u16()) o.image16 = previewImage_.u16();
        else o.image = previewImage_.u8();
        if (previewTransform_) o.transform = *previewTransform_;
        const Layer* layer = document_->find(*previewLayerId_);
        if (layer && layer->mask && !layer->mask->placement && previewTransform_ && !previewTransform_->samePlacement(layer->transform)) o.maskPlacement = std::optional<LayerTransform>(layer->transform);
    }
    auto strokeOverride = [&](const BrushStroke& stroke, const Uuid& layerId, bool mask) {
        LayerOverride& o = overrides[layerId];
        const Layer* layer = document_ ? document_->find(layerId) : nullptr;
        if (mask) {
            o.maskImage = stroke.previewMask();
            if (layer && layer->mask && layer->mask->placement) o.maskPlacement = std::optional<LayerTransform>(stroke.paintTransform());
        } else {
            o.image = stroke.previewImage();
            o.transform = stroke.paintTransform();
            // A mask covering the old grid stays where it was while the layer grows under the edit.
            if (layer && layer->mask && !layer->mask->placement && layer->asset) o.maskPlacement = std::optional<LayerTransform>(layer->transform);
        }
    };
    if (stroke_) strokeOverride(*stroke_, strokeLayerId_, strokeMask_);
    if (gradient_) strokeOverride(*gradient_->raster, gradient_->layerId, gradient_->mask);
    if (pixelMove_) strokeOverride(*pixelMove_->raster, pixelMove_->layerId, false);
    if (warp_ && document_) {
        LayerOverride& o = overrides[warpLayerId_];
        o.image = warp_->image();
        o.transform = LayerTransform(Point(0, 0), document_->size());
        const Layer* layer = document_->find(warpLayerId_);
        if (layer && layer->mask && !layer->mask->placement) o.maskPlacement = std::optional<LayerTransform>(layer->transform);
    }
    if (filterMaskShown() && document_) {
        // Alt-click on the filter mask: the mask (as it is being painted) in gray over everything.
        const Layer* proxy = document_->find(*filterMaskLayer_);
        auto it = overrides.find(proxy->id);
        GrayPtr mask = it != overrides.end() && it->second.maskImage ? *it->second.maskImage : (proxy->mask ? proxy->mask->asset.image.u8() : nullptr);
        if (mask) {
            if (filterMaskView_.first != mask || stroke_ || gradient_) {
                auto gray = std::make_shared<Image>(mask->width(), mask->height());
                for (int y = 0; y < mask->height(); y++) {
                    const uint8_t* m = mask->row(y);
                    for (int x = 0; x < mask->width(); x++) { uint8_t* p = gray->pixel(x, y); p[0] = p[1] = p[2] = m[x]; p[3] = 255; }
                }
                filterMaskView_ = {mask, gray};
            }
            if (!filterMaskWhite_ || filterMaskWhite_->width() != mask->width() || filterMaskWhite_->height() != mask->height())
                filterMaskWhite_ = std::make_shared<GrayImage>(mask->width(), mask->height(), 255);
            LayerOverride& o = overrides[proxy->id];
            o.image = filterMaskView_.second;
            o.maskImage = filterMaskWhite_;
            if (proxy->mask && proxy->mask->placement) o.transform = *proxy->mask->placement;
        }
    }
    return overrides;
}

} // namespace app
