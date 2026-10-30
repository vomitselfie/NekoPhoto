// The editor's state and every edit it can make: the document, its history,
// the active layer, the tools and their settings. A port of the parts of
// Document/EditorSession.swift (and its extensions) that the Linux UI needs.
// Views observe it through signals and never mutate the document themselves.
// The implementation is split by area, like the Swift extensions: EditorSession.cpp (document, history,
// canvas, view) and EditorSession{Layers,Transform,Painting,Pixels,Selection,QuickSelect,Adjustments}.cpp.
// Its plain types are in headers of their own (EditorTool.h, EditorToolSettings.h, EditorTransform.h, EditorProject.h), and
// what its declarations only name is forward-declared: a header that only names the session forward-declares it too,
// so that only the files that use it include this one.
#pragma once
#include "EditorProject.h"
#include "EditorTool.h"
#include "EditorToolSettings.h"
#include "EditorTransform.h"
#include "Viewport.h"
#include "nekophoto/brush.h"
#include "nekophoto/brushsmoothing.h"
#include "nekophoto/colormgmt.h"
#include "nekophoto/document.h"
#include "nekophoto/layerstyle.h"
#include "nekophoto/toning.h"
#include "nekophoto/vectorlayer.h"
#include "nekophoto/history.h"
#include "nekophoto/modetransform.h"
#include "nekophoto/render.h"
#include "nekophoto/selection.h"
#include "nekophoto/channels.h"
#include "nekophoto/smartwand.h"
#include "nekophoto/shape.h"
#include "nekophoto/warp.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <functional>
#include <map>
#include <QColor>
#include <QObject>
#include <QTimer>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <memory>
#include <optional>
#include <set>
#include <thread>

class QFileSystemWatcher;

// What the declarations below only name (their headers come with the files that use them).
namespace nekophoto {
struct AdjustmentSettings;
struct CameraRawSettings;
struct PatternPreset;
struct SmartFilterEntry;
struct SmartFilterStack;
struct TrimOptions;
class MyPaintStroke;
class TipStroke;
class WarpStroke;
} // namespace nekophoto

namespace app {

struct DocumentSource;

class EditorSession : public QObject {
    Q_OBJECT
    mutable std::pair<const void*, std::shared_ptr<const nekophoto::GrayImage>> selection8_;
public:
    explicit EditorSession(QObject* parent = nullptr);
    ~EditorSession() override;

    // Document
    const std::optional<nekophoto::Document>& document() const { return document_; }
    bool hasDocument() const { return document_.has_value(); }
    QString projectPath() const { return projectPath_; }
    QString title() const;
    bool isModified() const { return history_.isModified(); }
    /// A document that exists nowhere else (one recovered after a crash): unsaved until saved.
    void markUnsaved() { history_.markUnsaved(); emit titleChanged(); emit historyChanged(); }
    /// The document is the file it was just opened from: no history behind it and nothing unsaved (Photoshop opens a
    /// PNG or a RAW file unmodified), so closing it asks nothing and Revert has nothing to go back to yet.
    void markOpened(const QString& name = {}) { if (!name.isEmpty()) importedName_ = name; history_.reset(); history_.markSaved(); emit titleChanged(); emit historyChanged(); }
    void createDocument(int width, int height, double resolution = 72, bool emptyLayer = true);
    /// A document made here and not touched since (File > New, document.new): nothing to undo or redo, never saved,
    /// opened, pasted or changed, no selection, guides, channels or slices, and no layer or one plain pixel layer whose
    /// pixels are all alike (none painted yet, or one colour). A drop or a paste then opens in its place rather than as a
    /// layer of it (MainWindowIncoming.cpp).
    bool isUntouchedBlank() const;
    /// A project read from disk with its saved active layer, before any session takes it.
    using LoadedProject = app::LoadedProject;   // EditorProject.h
    /// Reads a project without touching a session, so a failed open never disturbs a tab.
    static std::optional<LoadedProject> readProject(const QString& path, QString* error);
    void installProject(LoadedProject project);
    bool openProject(const QString& path, QString* error);
    bool saveProject(const QString& path, QString* error);
    /// The open project changed on disk (another app, an agent writing the package): after `resolveExternalChange`,
    /// true reloads it in place (losing unsaved work), false keeps what is open.
    void resolveExternalChange(bool revert);
    /// Reloads that followed the package on disk (for tests and automation).
    int externalReloads() const { return externalReloads_; }
    void closeDocument();
    /// Adds imported pixels as a new layer, centred on `at` (or the canvas); a first import creates the canvas, at the
    /// image's depth (a 16-bit PNG opens as a 16-bit document). Into an existing document they take its depth.
    /// False, saying why (in `errorText` when given, else through `error`), when the image would break a budget rule.
    /// Pixels from elsewhere (the clipboard, another document, a file in `profile`) in this document's mode: converted
    /// through the profiles when the mode differs, as Photoshop converts a paste or a placed file; into CMYK and Lab also
    /// at this document's depth. RGB into RGB is returned as it is (the callers convert profile and depth as before).
    nekophoto::AnyImage pixelsForDocument(const nekophoto::AnyImage& image, nekophoto::ColorMode mode, const nekophoto::ColorProfile& profile) const;
    bool insertImage(const nekophoto::AnyImage& image, const QString& name, std::optional<QPointF> at = std::nullopt, QString* errorText = nullptr);
    /// A refused budget check (Document::canCreate and the rest) in the reader's language.
    static QString budgetText(const nekophoto::BudgetCheck& check);
    /// The composite at 8 bits: a 16-bit document's is dithered down (for the 8-bit formats and the clipboard).
    std::shared_ptr<nekophoto::Image> flattened() const;
    /// The composite at the document's depth (16-bit exports); an 8-bit document's widened. A 32-bit document's is
    /// tone-mapped at exposure 0 and encoded through its curve, as 32-bit exports to 8 and 16 bits are.
    std::shared_ptr<nekophoto::Image16> flattened16() const;
    /// A 32-bit document's composite in linear float (32-bit files); an 8- or 16-bit one's linearised.
    std::shared_ptr<nekophoto::ImageF> flattenedF() const;

    // Bit depth (docs/bit-depth.md)
    nekophoto::SampleType sampleType() const { return document_ ? document_->sampleType : nekophoto::SampleType::U8; }
    /// Image > Mode: RGB, CMYK or Lab (RGB with no document).
    nekophoto::ColorMode colorMode() const { return document_ ? document_->colorMode : nekophoto::ColorMode::RGB; }
    /// Whether the document's depth or mode can refuse features (anything but 8-bit RGB).
    bool featuresGated() const { return sampleType() != nekophoto::SampleType::U8 || colorMode() != nekophoto::ColorMode::RGB; }
    /// The tooltip of an item greyed for the document: "Not available in CMYK mode", or "Not available in 16-bit yet".
    QString unavailableTip() const;
    /// Image > Mode > RGB Color, CMYK Color or Lab Color (docs/color-modes.md): every layer and stored colour, and the
    /// foreground and background colours, converted with Color Settings' conversion options, one undo step.
    bool convertColorMode(nekophoto::ColorMode mode, QString* errorText = nullptr);
    /// The same for one feature: at 32 bits "Not available in 32-bit mode" for what Photoshop lacks there, "Not
    /// available in 32-bit yet" for what is not ported.
    QString unavailableTip(std::string_view feature) const;
    /// Image > Mode > 8, 16 or 32 Bits/Channel: every layer, mask and the selection converted, one undo step. From 32
    /// bits `toning` is HDR Toning's settings (none: the values as they are). False, with `error` saying why (a 16-bit
    /// document holds half the pixels within the same memory, a 32-bit one a quarter), when it cannot.
    bool convertMode(nekophoto::SampleType type, QString* error = nullptr, const nekophoto::View32* toning = nullptr);
    /// A 32-bit document's view (View ▸ 32-bit Preview Options, the status bar's exposure): what the canvas shows, not
    /// the pixels; not an undo step. Each tab keeps its own.
    const nekophoto::View32& view32() const { return view32_; }
    void setView32(const nekophoto::View32& view);
    /// The document's brightest luminance, for Highlight Compression's white (cached per document revision).
    float documentPeak();
    // Colour management (EditorSessionColor.cpp, docs/color-management.md).
    /// Edit > Assign Profile: the document's profile only, no pixel changes; one undo step. Empty: untagged (sRGB).
    bool assignProfile(const nekophoto::ColorProfile& profile);
    /// Edit > Convert to Profile: every raster, the stored colours and the foreground and background colours
    /// converted; one undo step. False, with `error`, when it cannot.
    bool convertToProfile(const nekophoto::ColorProfile& profile, const nekophoto::ConvertOptions& options, QString* error = nullptr);
    /// The profile a document takes as it opens or is made (no undo step: it is part of the opening).
    void adoptProfile(const nekophoto::ColorProfile& profile);
    /// Whether `feature` (nekophoto/supports.h) works on this document: every feature does on an 8-bit one.
    bool supportsFeature(std::string_view feature) const;
    /// When it does not: says so, "<what> is not available for 16-bit documents yet" (in `errorText` when given, else
    /// through `error`, which automation returns), and returns true.
    bool refusedAtDepth(std::string_view feature, const QString& what, QString* errorText = nullptr);
    /// The selection's coverage at 8 bits, for drawing its outline and testing a point (a 16-bit selection reduced,
    /// once per selection).
    std::shared_ptr<const nekophoto::GrayImage> selectionCoverage8() const;
    /// The same for any selection (the one displayed while it moves, say).
    static std::shared_ptr<const nekophoto::GrayImage> coverage8(const nekophoto::Selection& selection);

    // Layers
    std::optional<nekophoto::Uuid> activeLayerId() const { return activeLayerId_; }
    const std::set<nekophoto::Uuid>& selectedLayerIds() const { return selectedLayerIds_; }
    const nekophoto::Layer* activeLayer() const;
    bool isMaskSelected() const { return isMaskSelected_; }
    /// Whether painting now goes into a mask: a layer's (an adjustment's, a group's), the Quick Mask or an alpha channel.
    bool paintsMask() const;
    /// The grey a colour paints on a mask, 0..1: Photoshop's luminosity, 0.30 R + 0.59 G + 0.11 B.
    static double maskGrey(const QColor& color);
    /// What `color` puts into the mask being painted: its grey, the inverse on the Quick Mask and an alpha channel
    /// (their layers' masks hold the inverse of what they select).
    double maskPaintValue(const QColor& color) const;
    void selectLayer(const std::optional<nekophoto::Uuid>& id, bool mask = false);
    void selectLayers(const std::set<nekophoto::Uuid>& ids, const std::optional<nekophoto::Uuid>& primary);
    std::set<nekophoto::Uuid> collapsedGroupIds;
    void toggleGroupExpansion(const nekophoto::Uuid& id);
    /// A blank layer above the active one (inside it, for a folder), or `below` it.
    void addBlankLayer(bool below = false);
    void addGroup();
    void groupSelectedLayers();
    void deleteLayer(const nekophoto::Uuid& id);
    void deleteSelectedLayers();
    /// Layers that clip to something being deleted but survive it; the UI asks whether to bake or release them.
    std::vector<nekophoto::Uuid> clippingDependents(const std::vector<nekophoto::Uuid>& ids) const;
    /// Deletes `ids`; with `bake`, dependents keep their masked appearance in their pixels first.
    void deleteLayersResolvingClipping(const std::vector<nekophoto::Uuid>& ids, bool bake);
    void duplicateActiveLayer();
    /// Ctrl+E: one layer merges with the one beneath it; several selected merge together; a folder merges its contents.
    struct MergePlan { std::vector<nekophoto::Uuid> ids; std::set<nekophoto::Uuid> removed; std::string name; std::optional<nekophoto::Uuid> parent; nekophoto::Uuid anchor; QString action; };
    std::optional<MergePlan> mergePlan() const;
    bool canMergeLayers() const;
    QString mergeTitle() const;
    void mergeLayers();
    /// Layer > Merge Visible (Ctrl+Shift+E): every layer shown on the canvas merges into one; hidden layers stay.
    std::optional<MergePlan> mergeVisiblePlan() const;
    bool canMergeVisible() const;
    void mergeVisible();
    void moveActiveLayerOutOfGroup();
    /// Alt-drag in the Layers panel: a copy of the layer placed where it was dropped.
    bool duplicateLayerTo(const nekophoto::Uuid& id, const std::optional<nekophoto::Uuid>& parent, const std::optional<nekophoto::Uuid>& above, bool atBottom);
    /// Alt-dragging a mask thumbnail onto another layer: a copy of the mask, where it sits on the document.
    bool copyMask(const nekophoto::Uuid& source, const nekophoto::Uuid& target);
    void mergeDown();
    void renameLayer(const nekophoto::Uuid& id, const QString& name);
    void toggleLayerVisibility(const nekophoto::Uuid& id);
    /// The Layers panel's eyes: a press toggles the eye and a drag across others sets them alike, shown as it goes;
    /// the release makes it one undo step. One eye changed is layers.set (visible) when commands are routed.
    void beginVisibilitySwipe(const nekophoto::Uuid& id);
    void setVisibilityInSwipe(const nekophoto::Uuid& id, bool visible);
    void endVisibilitySwipe();
    void moveActiveLayer(int offset);
    bool canMoveActiveLayer(int offset) const;
    bool placeLayer(const nekophoto::Uuid& id, const std::optional<nekophoto::Uuid>& parent, const std::optional<nekophoto::Uuid>& above, bool atBottom = false);
    /// Whether placeLayer would move the layer there.
    bool canPlaceLayer(const nekophoto::Uuid& id, const std::optional<nekophoto::Uuid>& parent, const std::optional<nekophoto::Uuid>& above, bool atBottom = false) const;
    void setLayerOpacity(double opacity);
    /// The selected layers' Fill (Photoshop's Fill Opacity): their own pixels fade, their effects do not. Opacity and
    /// Fill never change each other.
    void setLayerFill(double fill);
    /// A drag of the Opacity slider (or with `fill`, the Fill slider) as one undo step.
    void beginOpacityEdit(bool fill = false);
    void endOpacityEdit();
    /// The Layers panel's opacity (or with `fill`, Fill) slider dragged while commands are routed: the selected layers
    /// show each value with no undo step, and the release puts them back and answers the value to commit
    /// (layers.set), or none when it did not change. Another edit starting mid-drag commits it first, as
    /// endOpacityEdit does.
    void beginOpacityPreview(bool fill = false);
    void previewLayerOpacity(double opacity);
    std::optional<double> endOpacityPreview();
    bool opacityPreviewing() const { return opacityPreview_.has_value(); }
    void setLayerBlendMode(nekophoto::BlendMode mode, bool passThrough = false);
    /// Hovering the blend menu: the active layer drawn in `mode` until the menu closes.
    void previewBlendMode(std::optional<nekophoto::BlendMode> mode);
    void toggleClippingMask(const nekophoto::Uuid& id);
    bool canToggleClippingMask(const nekophoto::Uuid& id) const;
    void addLayerMask(bool revealing);
    /// Layer > Layer Mask > Reveal Selection (white inside the selection, black outside) or Hide Selection; without a
    /// selection, Reveal All or Hide All.
    void addMaskFromSelection(bool revealSelection);
    void toggleLayerMask();
    void deleteLayerMask();
    void toggleMaskLink(const nekophoto::Uuid& id);
    void applyMask();
    void invertMask();
    void flipLayer(bool horizontal);
    void flipCanvas(bool horizontal);
    void setLayerSampling(nekophoto::Sampling sampling);
    bool canEditLayers() const;

    // Transform (Move tool)
    const std::optional<TransformEdit>& transformEdit() const { return transformEdit_; }
    bool canTransform() const;
    /// `selectMoveTool` false: the session's tool stays (a drag with the Move tool held by Ctrl from another tool).
    void beginTransform(bool persistent, bool selectMoveTool = true);
    void previewTransform(const nekophoto::LayerTransform& value);
    void commitTransform();
    void cancelTransform();
    /// One command path (CONTRIBUTING.md, "Commands"): the menus, shortcuts, dialogs and canvas gestures that have
    /// been converted run their edit as the automation method that does it, through this runner, so the socket,
    /// --call, --batch, Actions playback and the interface share one implementation and Actions record the same
    /// step. MainWindow installs it on every tab's session; the result, or none after an error it has shown.
    std::function<std::optional<QJsonValue>(const QString& method, const QJsonObject& params)> commandRunner;
    std::optional<QJsonValue> runCommand(const QString& method, const QJsonObject& params = {});
    /// Whether runCommand reaches the registry for this session now (it does for the tab on screen); when it does
    /// not, callers make the edit directly, as they did before the command path.
    std::function<bool()> commandReady;
    bool commandsRouted() const { return commandRunner && (!commandReady || commandReady()); }
    /// A panel's command: the method when it is routed, else `direct` (the edit as before the command path).
    template <class F> void runCommandOr(const QString& method, const QJsonObject& params, F&& direct) {
        if (commandsRouted()) runCommand(method, params);
        else direct();
    }
    /// Free Transform's commit stage (Enter, the options bar's Apply, a double-click): the transform is invoked
    /// (transformCommand), updated interactively on the canvas (previewTransform), then committed here. A plain
    /// layer transform commits as the command layers.setTransform with the box's final values, the same call
    /// automation makes; a distortion, a folder, several layers, a mask alone or selected pixels (which that method
    /// cannot express) commit directly, as before.
    void commitTransformCommand();
    void nudgeLayer(double dx, double dy);
    /// Ctrl+T: transforms the selected pixels when there is a selection, else the layer(s).
    void transformCommand();
    bool canTransformSelection() const;
    void beginSelectionTransform();
    /// Alt-drag: a copy of the active layer is made and moved; cancelling removes it again.
    void beginDuplicateTransform(bool selectMoveTool = true);
    /// Several selected layers, or a folder: the transform moves them together in one box.
    bool transformsAsGroup() const;
    std::vector<const nekophoto::Layer*> groupTransformMembers() const;
    std::optional<nekophoto::LayerTransform> groupTransformBox() const;
    /// Ctrl-drag on a handle: the corners start moving freely; Apply resamples the pixels.
    void beginDistort();
    void previewCorners(const nekophoto::Corners& corners);
    /// Where the transform box's corners are right now (the distortion's, or the draft's).
    nekophoto::Corners editedCorners(const nekophoto::Layer& layer) const;
    /// The transform a layer shows right now: the pending draft or its own.
    nekophoto::LayerTransform displayedTransform(const nekophoto::Layer& layer) const;
    nekophoto::LayerTransform editedTransform(const nekophoto::Layer& layer) const;
    bool showsTransformControls = true;
    bool locksTransformRatio = true;
    bool transformAutoSelect = false;
    /// Guides the last move snapped to, for the canvas to draw.
    std::vector<double> snapGuidesX, snapGuidesY;
    void setSnapGuides(std::vector<double> xs, std::vector<double> ys);
    /// The topmost visible layer with pixels under a document point.
    std::optional<nekophoto::Uuid> layerAt(QPointF documentPoint) const;
    /// Every visible layer with pixels under a document point, topmost first (the Move tool's context menu).
    std::vector<nekophoto::Uuid> layersAt(QPointF documentPoint, size_t limit = 64) const;
    /// Pixels the transform places (what 100% draws 1:1).
    std::optional<nekophoto::Size> transformPixelSize() const;

    // Brush
    nekophoto::BrushSettings brushSettings;
    /// The Brush tool's preset (BrushLibrary id: "classic/pencil", "imported/<set>/<brush>"), or empty for the
    /// round tip. A MyPaint preset paints layer pixels only (a mask gets the round tip); a tip brush paints
    /// masks too.
    QString brushPreset;
    /// The pen as the canvas last saw it, raw (brushsample.h): pressure, tilt in degrees, twist, tangential pressure,
    /// the event time in seconds and whether a stylus sent it; a mouse sends neutral values. The brush takes its
    /// position from the point it is given.
    nekophoto::BrushSample pen;
    /// Smoothing for the Brush tool's strokes (brushsmoothing.h): input smoothing, the stabiliser (the options bar's
    /// Smoothing) with its modes, and pressure smoothing. All 0 by default: the pen is followed as it came.
    nekophoto::BrushSmoothing brushSmoothing;
    /// The seed of the next stroke's tip-brush jitter (a replayed stroke); a fresh one per stroke when unset.
    std::optional<uint32_t> brushSeed;
    /// Photoshop's opacity keys: 1 = 10% ... 9 = 90%, 0 = 100%; two digits typed quickly set an exact value. With
    /// `fill` (Shift and the digits, with the Move tool) they set the selected layers' Fill instead.
    void typeOpacityDigit(int digit, bool fill = false);
    void changeBrushHardness(bool increase);
    void changeBrushSize(bool increase);
    /// The Blur tool's modes; Liquify and Smudge push pixels, Blur paints a softened copy and Sharpen a sharpened one.
    BlurToolMode blurMode = BlurToolMode::Liquify;
    /// Dodge, Burn and Sponge (one tool, as Photoshop's O group); the brush's opacity is their Exposure or Flow.
    nekophoto::ToningSettings toning;
    bool beginToning(QPointF documentPoint);
    /// Paint Bucket: fills the pixels like the one clicked (the wand's test, on the active layer or all layers) with
    /// the foreground colour at the brush's opacity, inside the selection.
    struct BucketSettings { int tolerance = 32; bool contiguous = true, antialias = true, allLayers = false; };
    BucketSettings bucket;
    bool paintBucket(QPointF documentPoint);
    /// Patch (the healing tool's Patch mode): the selection's pixels on the active layer replaced by those `dx`, `dy`
    /// away, their tone matched to the selection's edge (Photoshop's Patch, Source mode).
    bool patchSelection(int dx, int dy);
    bool warpActive() const { return warp_ != nullptr; }
    bool beginWarp(QPointF documentPoint);
    void continueWarp(QPointF documentPoint);
    void endWarp();
    void cancelWarp();

    // Gradient tool
    GradientSettings gradientSettings;
    bool gradientPending() const { return gradient_ != nullptr; }
    std::optional<std::pair<QPointF, QPointF>> gradientLine() const;
    void beginGradient(QPointF documentPoint);
    void moveGradient(QPointF end);
    void endGradientDrag();
    void refreshGradient();
    void commitGradient();
    void cancelGradient();
    /// Switching tools, layers or targets applies the pending gradient, as in Photoshop.
    void resolveGradient();

    // Text tool. New text takes `textStyle` (its colour follows the foreground colour) and opens the editor.
    nekophoto::LayerText textStyle;
    /// Adds a text layer above the active one with its top-left near `documentPoint`; with `openEditor` the
    /// text editor is requested for it. Returns the layer's id, or none when nothing could be added.
    std::optional<nekophoto::Uuid> addTextLayer(QPointF documentPoint, const nekophoto::LayerText& text, bool openEditor);
    std::optional<nekophoto::LayerText> layerText(const nekophoto::Uuid& id) const;
    /// Text edits: a session (the dialog) applies changes as they come and keeps or drops them at the end;
    /// outside a session each setLayerText is its own undo step.
    void beginTextEdit(const nekophoto::Uuid& id);
    void setLayerText(const nekophoto::Uuid& id, const nekophoto::LayerText& text);
    void endTextEdit(bool keep);
    bool textEditing() const { return textEditing_; }
    /// Typing on the canvas (EditorSessionTextEdit.cpp): a session on a live text layer, or on a new one at `point`
    /// (the first baseline's start; with `box`, paragraph text in a box from `point`), held open as one undo step
    /// until endTypeEdit. The text is set exactly (its runs as given) and stays anchored: point text on its first
    /// baseline at its alignment's side, box text at the box's corner, moved by `rasterShift` (raster pixels) when a
    /// box edge was dragged. Other edits wait meanwhile (canEditLayers is false).
    bool beginTypeEdit(const nekophoto::Uuid& id);
    std::optional<nekophoto::Uuid> beginNewTypeEdit(QPointF point, const nekophoto::LayerText& style, std::optional<QSizeF> box);
    bool setTypeEditText(const nekophoto::LayerText& text, QPointF rasterShift = {});
    void endTypeEdit(bool keep);
    std::optional<nekophoto::Uuid> typeEditLayer() const { return typeEdit_ ? std::optional(typeEdit_->layer) : std::nullopt; }
    void requestTextEdit(const nekophoto::Uuid& id) { emit textEditRequested(id); }

    // Paths: the Pen (P) and Direct Selection (A) tools work on the target path: the path chosen in the Paths panel,
    // or else the active vector shape layer's. Paths themselves are the document's (vectorlayer.h DocumentPath).
    enum class PenMode { Shape, Path };
    PenMode penMode = PenMode::Shape;
    /// Photoshop's path operation for the next outline the Pen or the Shape tool adds: none (New Layer) makes a new
    /// shape layer; Combine, Subtract Front Shape, Intersect or Exclude add it to the target (the active shape layer,
    /// the targeted vector mask, or in Path mode the path) as a new component combined that way.
    std::optional<nekophoto::VectorPath::Op> pathOp;
    /// The subpath Direct Selection last picked on the target path (its component's operation is the bar's).
    std::optional<int> selectedSubpath() const { return selectedSubpath_; }
    void setSelectedSubpath(std::optional<int> index);
    /// The picked subpath's component combined by `op` instead, as one undo step.
    bool setSelectedSubpathOp(nekophoto::VectorPath::Op op);
    /// Merge Shape Components on the target path (add-only geometry), as one undo step.
    bool mergeTargetComponents();
    void mergeWithPlan(const MergePlan& plan);
    /// Photoshop's Auto Add/Delete: with no path being drawn, the Pen adds an anchor on the target path's outline and
    /// deletes one it clicks.
    bool penAutoAddDelete = true;
    /// An anchor added on the target path's outline nearest `documentPoint` (within `radius` pixels), or the anchor
    /// there deleted; one undo step each. False when nothing was near.
    bool addAnchorAt(QPointF documentPoint, double radius);
    bool deleteAnchorAt(QPointF documentPoint, double radius);
    const std::optional<nekophoto::VectorPath::Subpath>& penDraft() const { return penDraft_; }
    /// A click (a corner knot) or the start of a drag (a smooth one whose handles follow penDrag).
    void penPress(QPointF documentPoint);
    void penDrag(QPointF documentPoint);
    /// Ends the path being drawn: closed, or left open (Enter); cancelled with Esc.
    void penFinish(bool close);
    void penCancel();
    /// The active layer's own vector mask as the target (clicking its thumbnail in the Layers panel); the Paths
    /// panel's choice is let go. False when the layer has no vector mask.
    bool targetVectorMask(const nekophoto::Uuid& layer);
    bool vectorMaskTargeted() const;
    /// Layer > Vector Mask: Reveal All, Hide All (an empty path, inverted), Current Path (the target path, or the
    /// Paths panel's), and Delete; one undo step each.
    enum class VectorMaskKind { RevealAll, HideAll, CurrentPath };
    bool addVectorMask(VectorMaskKind kind, QString* error = nullptr);
    bool deleteVectorMask();
    /// The layer's vector mask made or replaced by `path` (document pixels), as one undo step (automation).
    bool setVectorMaskPath(const nekophoto::Uuid& layer, const nekophoto::VectorPath& path, QString* error = nullptr);
    /// Type > Create Work Path and Convert to Shape: a text layer's glyph outlines (as it is laid out upright) as the
    /// Work Path, or as a new shape layer in place of the text layer (hidden, as Photoshop replaces it).
    bool textToWorkPath(const nekophoto::Uuid& layer, QString* error = nullptr);
    bool textToShape(const nekophoto::Uuid& layer, QString* error = nullptr);
    /// The active shape layer's live properties (vectorlayer.h LiveShape), and one changed (its group redrawn) as
    /// one undo step.
    std::vector<nekophoto::LiveShape> activeLiveShapes() const;
    bool setActiveLiveShape(const nekophoto::LiveShape& shape);
    /// The Paths panel's choice (none: the active shape layer's path is the target).
    std::optional<uint16_t> activePathId() const { return activePathId_; }
    void selectPath(std::optional<uint16_t> id);
    std::vector<nekophoto::DocumentPath> paths() const;
    std::optional<nekophoto::VectorPath> targetPath() const;
    /// Live edits of the target path: begin, update as often as needed, end (one undo step).
    bool beginPathEdit(const QString& name);
    void updatePathEdit(const nekophoto::VectorPath& path);
    void endPathEdit();
    /// The target path replaced as one undo step.
    bool setTargetPath(const nekophoto::VectorPath& path, const QString& name);
    // The Paths panel's commands.
    uint16_t newPath(const QString& name);
    /// `path` stored as path `id` (0: a new saved path named `name`; kWorkPathId: the Work Path) as one undo step, and chosen.
    uint16_t storePath(uint16_t id, const QString& name, const nekophoto::VectorPath& path);
    void renamePath(uint16_t id, const QString& name);
    void deletePath(uint16_t id);
    /// The Work Path saved under `name`.
    void savePath(uint16_t id, const QString& name);
    bool pathToSelection(uint16_t id, nekophoto::SelectionMode mode);
    bool fillPath(uint16_t id);
    bool strokePath(uint16_t id);
    bool pathToShapeLayer(uint16_t id);
    /// The selection's outline as a new Work Path (traced along its half-coverage edge, simplified by `tolerance` pixels).
    bool selectionToWorkPath(double tolerance = 1.0);

    // Shape tool
    ShapeToolSettings shapeTool;
    /// The path the shape being dragged will have.
    std::optional<nekophoto::VectorPath> shapeDraftPath() const;
    /// The active layer's vector shape, when it is a vector shape layer; and a change to it as one undo step.
    std::optional<nekophoto::VectorShape> activeVectorShape() const;
    bool setActiveVectorShape(const nekophoto::VectorShape& shape, const QString& name);
    /// A new vector shape layer above the active one (named `name`, or after its kind), made active.
    bool addVectorShapeLayer(const nekophoto::VectorShape& shape, const QString& name = QString());
    const std::optional<ShapeDraft>& shapeDraft() const { return shapeDraft_; }
    void beginShape(QPointF documentPoint);
    void dragShape(QPointF documentPoint, bool square, bool fromCenter);
    void finishShape();
    void cancelShape();
    void toggleShapeKind();

    // Moving selected pixels (the Move tool with a selection)
    bool pixelMoveActive() const { return pixelMove_ != nullptr; }
    bool canMovePixels(QPointF documentPoint) const;
    bool beginPixelMove(bool duplicate);
    void movePixels(QPointF offset);
    void finishPixelMove();
    void cancelPixelMove();
    void nudgePixels(double dx, double dy);
    /// The outline to draw: during a pixel move or a floating transform, the selection carried along.
    std::optional<nekophoto::Selection> displayedSelection() const;
    /// The composite's straight colour under a document point, or none when transparent / outside.
    std::optional<QColor> compositeColorAt(QPointF documentPoint) const { return compositeColorAt(documentPoint, nullptr); }
    /// The same for many points over an unchanged document (the Eyedropper dragged): the composite is rendered into
    /// `snapshot` at the first sample and read from it after (32-bit, CMYK and Lab render the one pixel each time).
    using CompositeSnapshot = app::CompositeSnapshot;   // EditorToolSettings.h
    std::optional<QColor> compositeColorAt(QPointF documentPoint, CompositeSnapshot* snapshot) const;
    /// Hooks a panel can install to take the next canvas press (returns true to claim it), the drag and the release.
    std::function<bool(QPointF)> canvasPressHook;
    std::function<void(QPointF, QPointF)> canvasDragHook;
    std::function<void()> canvasReleaseHook;
    bool brushErase = false;
    /// The colours stay what they were picked as; on a mask (paintsMask) they paint their grey, as Photoshop's swatches
    /// show them there: the brush, the Gradient tool, the Paint Bucket and Edit > Fill the foreground's (or the
    /// colour asked for), the eraser the background's.
    QColor foregroundColor{Qt::black};
    QColor backgroundColor{Qt::white};
    bool brushActive() const { return stroke_ != nullptr; }
    bool beginBrush(QPointF documentPoint, bool straightFromLast);
    void continueBrush(QPointF documentPoint);
    void endBrush();
    void cancelBrush();
    std::optional<QPointF> lastBrushPoint() const { return lastBrushPoint_; }
    /// 0 Content-Aware, 1 Create Texture, 2 Proximity Match (Spot Healing); 3 Sampled: the Healing Brush, from the
    /// clone source (Alt-click); 4 Patch: drag the selection to where to copy from; 5 Content-Aware Move: drag the
    /// selection to where it should go.
    int spotHealingMode = 0;
    bool cloneAligned = true;
    bool cloneSampleAll = false;
    std::optional<QPointF> cloneSource;
    std::optional<QPointF> cloneOffset;
    void setCloneSource(QPointF documentPoint) { cloneSource = documentPoint; cloneOffset.reset(); emit toolChanged(); }
    /// Where Clone Stamp would copy from for a brush at `point`, for the canvas's crosshair.
    std::optional<QPointF> cloneSamplePoint(QPointF point) const;

    // Clipboard
    bool canCopyPixels() const;
    /// Edit > Copy, Copy Merged and Cut of pixels (pixels.copy, pixels.copyMerged, pixels.cut): false when there
    /// was nothing to copy. The system clipboard gets them at 8 bits, sRGB.
    bool copySelection();
    bool copyMerged();
    bool cutSelection();
    bool canPaste() const;
    /// Edit > Paste: copied layers (pasteLayers) when the layer clipboard holds them, else pixels (pastePixels).
    void paste();
    /// Pixels copied here or by another app, as a new layer (or into the one channel being edited), converted to
    /// the document's mode, profile and depth; false (with the reason in `error` when there is one) if nothing was
    /// pasted.
    bool pastePixels(QString* error = nullptr);
    bool hasPixelsToPaste() const;
    /// Edit > Copy with layers selected and no selection (EditorSessionClipboard.cpp): the selected layers and
    /// folders, with everything they hold, go to the layer clipboard every tab shares; other apps get them
    /// flattened. Paste in any document inserts them above the active layer, one undo step, converted to its
    /// profile and depth; the new layers' ids come back.
    bool copyLayers();
    static bool hasLayerClipboard();
    std::vector<nekophoto::Uuid> pasteLayers(QString* error = nullptr);
    void layerViaCopy();
    /// Content-Aware Fill of the selection on the active layer; the layer grows over any selection past its edge.
    /// The request chooses where it copies from and whether the result goes on a new layer.
    bool contentAwareFill(QString* error, const ContentFillRequest& request = {});
    /// The fill without committing it (the dialog's preview): the layer's new pixels (only the filled ones for a new
    /// layer) and where they sit.
    nekophoto::AnyImage contentAwareFillResult(const ContentFillRequest& request, nekophoto::LayerTransform& placed, QString* error) const;
    /// Content-Aware Move: the selected pixels move dx, dy (document pixels), the hole filled from its
    /// surroundings (Extend: the original stays); the selection follows. One undo step.
    bool contentAwareMove(int dx, int dy, QString* error);
    bool contentMoveExtend = false;
    int contentMoveAdaptation = 2;   // 0 very strict .. 4 very loose
    /// Content-Aware Scale of the active layer's pixels to `width` x `height` by seam carving (seamcarve.h); with
    /// `protectSelection`, the selected pixels are kept. The layer's origin stays; its size follows the pixels.
    bool contentAwareScale(int width, int height, bool protectSelection, QString* error);
    /// Dragging a layer between projects: `id` (a folder with its contents) copied from `source` into this
    /// document, centred on `at` (or the canvas); clipping to layers left behind is baked in. A first copy
    /// into an empty tab makes the canvas the source's size.
    bool copyLayerFrom(const EditorSession& source, const nekophoto::Uuid& id, std::optional<QPointF> at, QString* error);

    // Selection
    void applySelectionShape(const nekophoto::GrayImage& shape, nekophoto::SelectionMode mode, const QString& name);
    void applySelectionShape(const nekophoto::Gray16& shape, nekophoto::SelectionMode mode, const QString& name);
    void selectAll();
    void deselect();
    /// Select > Reselect: the selection the last Deselect dropped, back as one undo step.
    void reselect();
    bool canReselect() const;
    void invertSelection();
    void setSelection(const std::optional<nekophoto::Selection>& selection, const QString& name);
    /// `sampleRadius` 0, 1 or 2: the point, a 3x3 or a 5x5 average sets the colour to match (Photoshop's Sample Size).
    void magicWand(QPointF documentPoint, int tolerance, bool contiguous, bool sampleAllLayers, nekophoto::SelectionMode mode, int sampleRadius = 0, bool edgeAware = true, std::optional<bool> refineEdge = std::nullopt);
    /// Right after an edge-aware wand click, a new tolerance re-thresholds that click's field and replaces
    /// its Magic Wand step; otherwise it only sets the tolerance for the next click. True when it re-selected.
    bool retolerateWand(int tolerance);
    // Quick Select by scribble: strokes over the subject and over the background, segmented by GrabCut on the
    // flattened document and refined to its edges; the strokes stay until cleared, the last one wins where two overlap.
    struct Scribble { std::vector<QPointF> points; double size = 24; bool background = false; };
    int scribbleSize = 24;
    bool scribbleBackground = false;
    int scribbleRefine = 8;
    const std::vector<Scribble>& scribbles() const { return scribbles_; }
    /// Adds a stroke, and with `run` recomputes the selection from every stroke so far.
    void addScribble(const std::vector<QPointF>& points, bool background, bool run = true);
    void removeLastScribble();
    void clearScribbles();
    /// The selection from the strokes so far; false with `error` when none can be made (no foreground stroke, no OpenCV).
    bool runScribbleSelection(nekophoto::SelectionMode mode, QString* error = nullptr);
    // Quick Select's other engine: clicks for a prompt model (EfficientSAM). A click marks the subject, an
    // Alt-click what is not it, a drag a box; the prompts stay until cleared.
    struct ClickPrompt { QPointF at; int label = 1; };   // 1 subject, 0 not the subject, 2 and 3 a box's corners
    bool quickSelectClicks = false;                     // the engine in use: strokes, or clicks
    void setQuickSelectClicks(bool clicks);
    const std::vector<ClickPrompt>& clickPrompts() const { return clickPrompts_; }
    void addClickPrompt(QPointF at, bool background, bool run = true);
    void setClickBox(QPointF a, QPointF b, bool run = true);
    void removeLastClickPrompt();
    void clearClickPrompts();
    /// The selection from the prompts, on this thread; false with `error` when the model is missing or none can be made.
    bool runClickSelection(nekophoto::SelectionMode mode, QString* error = nullptr);
    /// The current engine's selection computed off the main thread and applied when done; `quickSelectBusy`
    /// meanwhile, and a change of strokes or prompts during a run queues another.
    void startQuickSelectJob();
    bool quickSelectBusy() const { return quickSelectBusy_; }
    void fillSelection(const QColor& color);
    void clearSelectionPixels();
    void selectionExpand(int amount);
    void selectionContract(int amount);
    /// Photoshop's Select > Modify: Feather (a Gaussian of that radius), Smooth (disc majority), Border (a band).
    void selectionFeather(double radius);
    void selectionSmooth(int radius);
    void selectionBorder(int width);
    /// Arrow keys with a selection tool: the outline moves by whole pixels, one undo step per press.
    void nudgeSelection(double dx, double dy);
    /// Load a layer's pixels (or its mask) as the selection.
    void loadLayerAsSelection(const nekophoto::Uuid& id, bool mask, nekophoto::SelectionMode mode);
    LassoKind lassoKind = LassoKind::Freehand;
    bool selectionAntialiased = true;
    /// The Marquee, Lasso and Magic Wand's mode, as the four buttons of Photoshop's options bar: what a new shape does
    /// with the selection there. Shift (add), Alt (subtract) and Shift+Alt (intersect) choose for one use.
    nekophoto::SelectionMode selectionToolMode = nekophoto::SelectionMode::Replace;
    /// The Marquee and Lasso's Feather in pixels (0..250, Photoshop's options bar field): each new shape is softened
    /// by it before it combines with the selection.
    double selectionToolFeather = 0;
    MarqueeKind marqueeKind = MarqueeKind::Rectangle;
    int wandTolerance = 32;
    bool wandContiguous = true;
    bool wandSampleAll = false;
    int wandSampleRadius = 0;
    /// Contiguous clicks follow the image (smartwand.h): perceptual colour, neighbour steps and texture.
    bool wandEdgeAware = true;
    /// The wand's edge is unmixed (smartwand.h refineWandEdge): the fringe of a line is partly selected, and
    /// Delete right after gives what is left the line's own colour instead of a rim of the background's.
    bool wandRefineEdge = true;

    // Adjustment layers
    /// A new adjustment layer above the active one, with a layer mask targeted (white, or the selection, which it
    /// takes), as Photoshop adds one.
    void addAdjustmentLayer(nekophoto::AdjustmentKind kind);
    /// Live edits of an adjustment layer's settings; wrap a slider drag in begin/end for one undo step.
    void beginAdjustmentEdit();
    void setAdjustment(const nekophoto::Uuid& id, const nekophoto::AdjustmentSettings& settings);
    void endAdjustmentEdit();
    std::optional<nekophoto::AdjustmentSettings> adjustmentSettings(const nekophoto::Uuid& id) const;

    // Layer styles (Layer ▸ Layer Style)
    /// A layer's style for editing (every effect, the ones switched off too); empty when it has none.
    nekophoto::LayerStyle layerStyle(const nekophoto::Uuid& id) const;
    bool canStyleLayer(const nekophoto::Uuid& id) const;
    /// Blending Options' Blend If (blendif.h) on any layer, folders and adjustment layers too: one undo step.
    bool setLayerBlendIf(const nekophoto::Uuid& id, const nekophoto::BlendIf& blendIf);
    /// Advanced Blending's Channels (blendif.h): the colour channels left out of the layer's blending, any layer kind;
    /// one undo step.
    bool setLayerExcludedChannels(const nekophoto::Uuid& id, uint8_t excluded);
    /// The Layer Style dialog's live edit: begin, show each change on the layer, then keep it as one undo step or put
    /// the layer back as it was (its carried style bytes untouched when nothing changed).
    bool beginLayerStyleEdit(const nekophoto::Uuid& id);
    /// The style shown on the layer while the dialog is open, with its Fill when `fill` is given (Cancel puts both back).
    void previewLayerStyle(const nekophoto::LayerStyle& style, std::optional<double> fill = std::nullopt);
    void endLayerStyleEdit(bool keep);
    /// Gives a layer `style` as one undo step named `step` (automation's layers.setStyle: Layer Style, Paste Layer Style
    /// or Clear Layer Style).
    bool applyLayerStyle(const nekophoto::Uuid& id, const nekophoto::LayerStyle& style, const char* step = "Layer Style");
    /// A style preset on a layer as one undo step ("Apply Style"): the document first gets the patterns it uses.
    bool applyStylePreset(const nekophoto::Uuid& id, const nekophoto::LayerStyle& style, const std::vector<nekophoto::PatternPreset>& patterns);
    /// Gives the document patterns it does not have (one undo step); how many were added.
    int addPatterns(const std::vector<nekophoto::PatternPreset>& patterns);
    /// Copy, Paste and Clear Layer Style on the active layer.
    void copyLayerStyle();
    bool canPasteLayerStyle() const { return styleClipboard_.has_value(); }
    /// The style Copy Layer Style holds (Layer > Layer Style > Copy Layer Style puts layers.style's answer here).
    const std::optional<nekophoto::LayerStyle>& styleClipboard() const { return styleClipboard_; }
    void setStyleClipboard(const nekophoto::LayerStyle& style) { styleClipboard_ = style; }
    void pasteLayerStyle();
    void clearLayerStyle();
    bool activeLayerHasStyle() const;

    // Quick Mask (Select ▸ Edit in Quick Mask Mode)
    bool quickMaskActive() const;
    /// The Quick Mask's layer while it is on (the Channels panel shows it).
    std::optional<nekophoto::Uuid> quickMaskLayerId() const { return quickMaskActive() ? quickMaskLayer_ : std::nullopt; }
    /// Whether painting now goes to the Quick Mask or an alpha channel (their layer masks hold the inverse of what they select).
    bool paintsQuickMask() const;
    void toggleQuickMask();
    bool beginQuickMask();
    /// Leaves Quick Mask, its mask becoming the selection; false when it was not on.
    bool endQuickMask();

    // Channels (Window ▸ Channels; EditorSessionChannels.cpp, docs/channels.md). The colour channels are views: the
    // ones edits write to (bits: red 1, green 2, blue 4; all three is the composite and the usual path) and the ones
    // the canvas shows. An alpha channel made the target is painted through a temporary layer, as Quick Mask is.
    /// Every colour channel of the document's mode as bits (RGB and Lab 7, CMYK 15): the composite.
    /// Fill in a CMYK or Lab document: `color` (sRGB, as colours are kept) in the document's mode over the active layer's
    /// pixels within the selection (P7 step D: the layer's own grid; a blank layer gets a canvas-sized one).
    /// `coverage` (document size) replaces the selection when given, times `opacity`; `from` (document size, the
    /// document's layout) is copied instead of the colour (Patch).
    /// The Paint Bucket in a CMYK or Lab document, at document pixel (x, y).
    bool paintBucketMode(const nekophoto::Layer& layer, int x, int y);
    bool fillThroughMode(const QColor& color, const char* name, const nekophoto::AnyGray* coverage = nullptr, double opacity = 1,
                         const nekophoto::AnyImage& from = {});
    unsigned allColors() const { return document_ ? nekophoto::colorChannelsAllFor(document_->colorMode) : nekophoto::colorChannelsAll; }
    unsigned activeColorChannels() const { return activeColors_; }
    unsigned visibleColorChannels() const { return visibleColors_; }
    const std::set<nekophoto::Uuid>& visibleAlphaChannels() const { return visibleAlpha_; }
    /// The alpha channel being edited, when one is.
    std::optional<nekophoto::Uuid> targetChannel() const;
    /// Clicking colour channels in the panel (`extend`: Shift-click adds them to the target).
    void selectColorChannels(unsigned bits, bool extend = false);
    /// Clicking an alpha or spot channel: it becomes the target and shows over the image (a spot channel only shows).
    bool selectAlphaChannel(const nekophoto::Uuid& id, bool extend = false);
    void setColorChannelVisible(unsigned bits, bool visible);
    void setAlphaChannelVisible(const nekophoto::Uuid& id, bool visible);
    /// What the canvas shows of the channels (applied after rendering; the default leaves the frame alone).
    nekophoto::ChannelView channelView() const;
    /// New Channel (black), Duplicate, Delete, rename and Channel Options, reorder: one undo step each.
    std::optional<nekophoto::Uuid> newChannel(const QString& name = {}, QString* error = nullptr);
    std::optional<nekophoto::Uuid> duplicateChannel(const nekophoto::Uuid& id, const QString& name = {}, QString* error = nullptr);
    bool deleteChannel(const nekophoto::Uuid& id);
    bool renameChannel(const nekophoto::Uuid& id, const QString& name);
    bool setChannelOptions(const nekophoto::Uuid& id, const QString& name, const QColor& color, double opacity, bool selectedAreas);
    bool moveChannel(const nekophoto::Uuid& id, int index);
    /// Select ▸ Save Selection: into a new channel (`into` empty; named `name`) or into `into` combined in `mode`.
    std::optional<nekophoto::Uuid> saveSelectionToChannel(const std::optional<nekophoto::Uuid>& into, const QString& name,
                                                           nekophoto::SelectionMode mode, QString* error = nullptr);
    /// Select ▸ Load Selection, and a Ctrl-click on a channel's thumbnail (thumbnailClickMode).
    bool loadSelectionFromSource(const nekophoto::SelectionSource& source, bool invert, nekophoto::SelectionMode mode, QString* error = nullptr);
    /// The temporary layer an alpha channel is painted through (hidden from the Layers panel, never written).
    bool isChannelProxy(const nekophoto::Uuid& layerId) const { return channelProxy_ && *channelProxy_ == layerId; }
    /// Stops editing an alpha channel (its layer goes); the composite is the target again.
    void endChannelEdit();

    // Destructive adjustments and filters on the active layer's pixels, inside the selection.
    bool canAdjustPixels() const;
    /// Shows `image` (placed by `transform`, or the layer's own) in place of a layer's pixels until cleared: `layerId`'s, or the active layer's at the time
    /// of the call. The preview stays with that layer whatever becomes active meanwhile.
    void setPixelPreview(nekophoto::AnyImage image, std::optional<nekophoto::LayerTransform> transform, std::optional<nekophoto::Uuid> layerId = std::nullopt);
    void clearPixelPreview();
    /// A layer's pixels, `layerId`'s or the active layer's (grown by `margin` layer pixels for blurs), and the
    /// transform placing them.
    std::shared_ptr<const nekophoto::Image> adjustmentSource(int margin, nekophoto::LayerTransform& transform, std::optional<nekophoto::Uuid> layerId = std::nullopt) const;
    /// The selection as coverage on that grid, or null when everything is selected.
    std::shared_ptr<nekophoto::GrayImage> selectionOnGrid(const nekophoto::LayerTransform& transform, int width, int height) const;
    /// The same in a 16-bit document.
    std::shared_ptr<const nekophoto::Image16> adjustmentSource16(int margin, nekophoto::LayerTransform& transform, std::optional<nekophoto::Uuid> layerId = std::nullopt) const;
    std::shared_ptr<nekophoto::Gray16> selectionOnGrid16(const nekophoto::LayerTransform& transform, int width, int height) const;
    /// The same in a 32-bit document, and the curve its colour is encoded with (encodedTransfer).
    std::shared_ptr<const nekophoto::ImageF> adjustmentSourceF(int margin, nekophoto::LayerTransform& transform, std::optional<nekophoto::Uuid> layerId = std::nullopt) const;
    std::shared_ptr<nekophoto::GrayF> selectionOnGridF(const nekophoto::LayerTransform& transform, int width, int height) const;
    /// The same at any depth and layout: a CMYK or Lab layer's own samples (modeedit.h), and the selection at the
    /// document's depth on their grid (null when everything is selected).
    nekophoto::AnyImage adjustmentSourceAny(int margin, nekophoto::LayerTransform& transform, std::optional<nekophoto::Uuid> layerId = std::nullopt) const;
    nekophoto::AnyGray selectionOnGridAny(const nekophoto::LayerTransform& transform, int width, int height) const;
    nekophoto::TransferCurve documentCurve() const;
    /// The composite at a document pixel as the document holds it (the Eyedropper in a 32-bit, CMYK or Lab document):
    /// `values` the straight native values (linear R, G, B at 32 bits; C, M, Y, K ink percentages; L, a, b), `color` the
    /// colour the pickers take (encoded through the document's curve at 32 bits, through the profile to sRGB in CMYK
    /// and Lab). None outside the canvas or on a transparent pixel.
    struct NativeSample { QColor color; std::vector<double> values; };
    std::optional<NativeSample> nativeColorAt(QPointF documentPoint) const;
    /// Replaces a layer's pixels as one undo step: `layerId`'s, or the active layer's. A dialog that opened on
    /// one layer passes that layer, so its result never lands on whatever was selected since.
    void commitPixels(nekophoto::AnyImage image, const nekophoto::LayerTransform& transform, const QString& name, std::optional<nekophoto::Uuid> layerId = std::nullopt);
    void invertActive();
    std::array<std::vector<double>, 4> activeHistogram() const;
    /// Levels' histograms of the active layer in a CMYK or Lab document: the composite (the inks' mean in CMYK, empty
    /// in Lab) and each channel as stored (C, M, Y, K; L, a, b), 256 bins.
    std::array<std::vector<double>, 5> activeHistogramNative() const;
    /// A temporary picture over the canvas that is never part of the document: the Levels and Curves clipping display
    /// while Alt is held on a black or white point. `image` lies on the document through `pixelToDocument`; `ground`
    /// fills the rest of the canvas.
    struct ViewOverlay {
        QImage image;
        nekophoto::Affine pixelToDocument;
        QColor ground;
    };
    const std::optional<ViewOverlay>& viewOverlay() const { return viewOverlay_; }
    void setViewOverlay(std::optional<ViewOverlay> overlay) {
        if (!overlay && !viewOverlay_) return;
        viewOverlay_ = std::move(overlay);
        emit viewOverlayChanged();
    }
    /// Remove Background: `mask` (white over the subject, on the layer's pixel grid) becomes the layer mask,
    /// multiplied with any mask already there; with a selection only the selected part changes. `pixels`, when
    /// given at the layer's size, replaces the layer's pixels in the same undo step (the edge colours after
    /// foreground estimation). `layerId` as for commitPixels. In an 8-bit CMYK or Lab document `pixels` are in the
    /// document's layout (the edge inks, or L, a and b).
    void applySubjectMask(std::shared_ptr<const nekophoto::GrayImage> mask, nekophoto::AnyImage pixels = {}, std::optional<nekophoto::Uuid> layerId = std::nullopt);
    /// The same on a 16-bit layer: a 16-bit mask, and its pixels at 16 bits (five samples in CMYK).
    void applySubjectMask(std::shared_ptr<const nekophoto::Gray16> mask, std::shared_ptr<const nekophoto::Image16> pixels, std::optional<nekophoto::Uuid> layerId = std::nullopt);
    /// `image` (pixels in this document's mode) as 8-bit sRGB, for deciding only: what the segmentation model and the
    /// matte refinement see. In CMYK and Lab the pixels converted through the document's profile (the model was trained
    /// on sRGB); in RGB the pixels themselves, reduced to 8 bits. Null for a 32-bit buffer or a conversion that fails.
    std::shared_ptr<const nekophoto::Image> analysisView(const nekophoto::AnyImage& image) const;

    // Crop / canvas
    void cropTo(const QRectF& rect, const char* action = QT_TRANSLATE_NOOP("History", "Crop"));
    /// Image ▸ Trim: the canvas cut to its content (transparency or a corner's colour); false when nothing would change
    /// or nothing would remain.
    bool trim(const nekophoto::TrimOptions& options);
    void resizeCanvas(int width, int height, double anchorX, double anchorY);
    void resizeImage(int width, int height, double resolution, int sampling = 2);

    // History
    bool canUndo() const;
    bool canRedo() const;
    QString undoName() const { return QString::fromStdString(history_.undoName()); }
    QString redoName() const { return QString::fromStdString(history_.redoName()); }
    std::vector<std::string> undoNames() const { return history_.pastNames(); }
    std::vector<std::string> redoNames() const { return history_.futureNames(); }
    void undo();
    void redo();
    /// Photoshop CC's Toggle Last State (Ctrl+Alt+Z): redoes the step just undone, otherwise undoes the last step.
    void toggleLastState();
    /// The file the document was opened from or last saved to, and how it was opened (DocumentSource.h): what
    /// File > Revert reads again. Opening and saving set it; Export leaves it.
    const DocumentSource& source() const { return *source_; }
    void setSource(DocumentSource source);
    /// File > Revert is available: the document came from a file and has changed since it was opened or saved.
    bool canRevert() const;
    /// File > Revert: the document as `loaded` (the source read again), in place, as one undo step named "Revert"
    /// (the steps before it stay; Undo brings back the document as it was). Unmodified afterwards, as just opened.
    /// False, saying why, when an edit in progress cannot be let go.
    bool revertTo(LoadedProject loaded, QString* error = nullptr);
    /// The layer a document taken from a file starts with active: the topmost visible pixel layer.
    static std::optional<nekophoto::Uuid> defaultActiveLayer(const nekophoto::Document& document);
    /// For grouping steps after the fact (the automation server's edit groups): the document's revision now,
    /// the revisions of the recorded steps, those recorded since a revision, and merging them into one step.
    uint64_t historyRevision() const { return history_.revision(); }
    /// Bytes only the undo history holds (DocumentHistory::retainedBytes); the work-counters self-test bounds it.
    size_t historyRetainedBytes() const { return history_.retainedBytes(document_); }
    std::vector<uint64_t> historyRevisions() const { return history_.pastRevisions(); }
    std::optional<std::vector<uint64_t>> historyRevisionsSince(uint64_t since) const { return history_.revisionsSince(since); }
    int squashHistory(uint64_t since, const QString& name);
    void beginEdit(const QString& name);
    void endEdit();

    // Timeline (frame animation, nekophoto/animation.h). Each change to the frames is one undo step; selecting
    // a frame is not (as in Photoshop), though it rewrites the layers' visibility, position and opacity.
    bool timelineCreate();
    /// Photoshop's Make Frames From Layers: a frame per top-level layer (the bottom one shown under each).
    bool timelineFramesFromLayers();
    bool timelineSelectFrame(int index);
    /// A copy of the current frame after it (Photoshop's New Frame).
    bool timelineDuplicateFrame();
    bool timelineDeleteFrame(int index);
    bool timelineMoveFrame(int from, int to);
    /// Delay in milliseconds; index -1 sets every frame's.
    bool timelineSetDelay(int index, int delayMs);
    bool timelineSetLoopCount(int loops);
    bool timelineClear();
    /// Playback: shows a frame without touching history or the frames; endFramePreview puts back the layer states
    /// playback began from (any edit, save or export does it first).
    void previewFrame(int index);
    void endFramePreview();
    bool previewingFrames() const { return previewBase_.has_value(); }
    /// The document as it really is: while playback shows a frame, a copy with the layers' own states back. What
    /// saves and autosaves write.
    nekophoto::Document documentToSave() const;

    // Tools and view
    Tool tool() const { return tool_; }
    /// Refused, with a message, for a tool that does not work at the document's depth.
    void selectTool(Tool tool);
    /// The supports() feature a tool is ("tool.move", "tool.brush", ...), and whether it works at the document's depth.
    static const char* toolFeature(Tool tool);
    bool toolSupportedAtDepth(Tool tool) const { return supportsFeature(toolFeature(tool)); }
    Viewport viewport;
    bool showsPixelGrid = true;
    void fitView();
    void zoomTo(double zoom, std::optional<QPointF> anchor = std::nullopt);
    /// The overrides the renderer needs while an edit is in progress.
    /// Takes an imported document (a PSD, say) as this session's, titled `name`, with no project path yet.
    void adoptDocument(const nekophoto::Document& document, const QString& name);
    /// A document recovered after a crash: unsaved, and, when it had been saved before, belonging to that project again,
    /// so Save writes it back to the same file or folder, in the same form (.nekophoto or .comp).
    /// `source`: where the document came from (DocumentSource::toJson), File > Revert's file.
    void adoptRecovered(const nekophoto::Document& document, const QString& name, const QString& originalPath, const QJsonObject& source = {});
    /// The name an imported document carries while it has no project path.
    const QString& importedName() const { return importedName_; }
    nekophoto::Overrides renderOverrides() const;
    /// Bumped on every document notification; what render caches key on.
    uint64_t documentRevision() const { return documentRevision_; }

    // ---- Artboards and slices (EditorSessionArtboards.cpp) -----------------------------------------------------
    /// The document's artboards (folders with an artboard), bottom to top.
    std::vector<const nekophoto::Layer*> artboards() const;
    /// A new, empty artboard at the top of the stack (one undo step); its id, or none when it cannot be added.
    std::optional<nekophoto::Uuid> addArtboard(const nekophoto::Artboard& artboard, const QString& name = {});
    /// An artboard's rectangle and background changed (one undo step); with `moveContents`, the layers inside
    /// follow the rectangle's move, as Photoshop moves an artboard with its contents.
    bool setArtboard(const nekophoto::Uuid& id, const nekophoto::Artboard& artboard, bool moveContents = true, const QString& name = {});
    /// The artboard's folder made a plain folder again, or with `contents` deleted with everything in it.
    bool removeArtboard(const nekophoto::Uuid& id, bool contents);
    /// The topmost artboard containing a document point.
    std::optional<nekophoto::Uuid> artboardAt(QPointF documentPoint) const;
    /// A new slice (its id is chosen when 0); one undo step.
    std::optional<uint32_t> addSlice(nekophoto::Slice slice);
    bool setSlice(const nekophoto::Slice& slice);
    bool deleteSlice(uint32_t id);

    // ---- Guides (EditorSessionArtboards.cpp) ----------------------------------------------------------------
    /// Ruler guides are document state: each add, move, removal or clearing is one undo step, named as
    /// Photoshop names it (New Guide, Move Guide, Delete Guide, Clear Guides). Positions are document pixels,
    /// rounded to the PSD's 1/32 pixel. The index of the new guide, or none without a document.
    std::optional<int> addGuide(nekophoto::Guide guide);
    bool moveGuide(int index, double position);
    bool removeGuide(int index);
    bool clearGuides();
    const std::vector<nekophoto::Guide>& guides() const;
    /// The guide within `tolerance` document pixels of a point (the nearest), for picking one up with the Move tool.
    std::optional<int> guideAt(QPointF documentPoint, double tolerance) const;

    /// The document rendered over `rect` (clipped to the canvas); null when nothing of it is on the canvas.
    std::shared_ptr<nekophoto::Image> renderRect(const QRect& rect) const;
    std::shared_ptr<nekophoto::Image16> renderRect16(const QRect& rect) const;
    /// File ▸ Export Artboards to Files / Export Slices: each one written to `directory` as `format` ("png" or "jpeg"),
    /// named `prefix` + its name. Returns the paths written; `error` says why one failed.
    /// `check`, when given, is asked about every file before anything is written; a reason it gives stops the export
    /// with nothing written. The prefix goes through the same file-name cleaning as the names.
    QStringList exportArtboards(const QString& directory, const QString& format, const QString& prefix, int quality, QString* error,
                                const std::function<QString(const QString&)>& check = {});
    QStringList exportSlices(const QString& directory, const QString& format, const QString& prefix, int quality, QString* error,
                             const std::function<QString(const QString&)>& check = {});

    // ---- Smart objects (EditorSessionSmartObjects.cpp) --------------------------------------------------------
    /// The selected layers as one smart object (one undo step).
    bool convertToSmartObject(QString* error);
    /// A file as a new embedded smart object above the active layer.
    bool placeEmbedded(const QString& path, QString* error);
    /// The active smart object's contents swapped for a file's, in every layer placing them.
    bool replaceSmartObjectContents(const QString& path, QString* error);
    /// The active smart object as plain pixels.
    bool rasterizeSmartObject();
    /// New Smart Object via Copy: the active smart object copied above it with contents of its own (one undo step).
    bool newSmartObjectViaCopy(QString* error);
    /// The active layer's camera RAW source (Camera Raw's Open Object) and its settings; null when it is not one.
    std::shared_ptr<const nekophoto::SmartObjectSource> activeRawSmartObject(nekophoto::CameraRawSettings* settings = nullptr) const;
    /// The active RAW smart object developed again with `settings` into `image`, in every layer placing it (one undo step).
    bool redevelopRawSmartObject(const nekophoto::CameraRawSettings& settings, const nekophoto::AnyImage& image, QString* error);
    /// Warp the active layer with a preset (Edit ▸ Warp): Warp Text on text, a baked mesh on a smart object, bent
    /// pixels otherwise; one undo step. False, with `error`, when the layer cannot take it.
    bool warpActiveLayer(const nekophoto::TextWarp& warp, QString* error = nullptr);
    /// The warp cage (Edit ▸ Warp Cage): a 4 x 4 mesh over the active layer, dragged on the canvas with a live preview,
    /// then applied (one undo step) or cancelled.
    bool beginWarpCage(QString* error = nullptr);
    const std::optional<nekophoto::WarpMesh>& warpCage() const { return warpCage_; }
    void moveWarpCagePoint(int index, QPointF documentPoint);
    void setWarpCage(const nekophoto::WarpMesh& cage);
    bool commitWarpCage(QString* error = nullptr);
    /// Enter on the warp cage (CONTRIBUTING.md, "Commands"): the cage's 16 points as layers.setCage on the layer it is
    /// over, when the command path reaches this session; otherwise, or when the method refuses, commitWarpCage.
    bool commitWarpCageCommand(QString* error = nullptr);
    void cancelWarpCage();
    /// Adds a Smart Filter on top of the active smart object's stack; one undo step.
    bool addSmartFilter(const nekophoto::SmartFilterEntry& entry, QString* error = nullptr);
    /// Whether the active layer is a smart object that can take Smart Filters (Photoshop's filter on a smart object).
    bool canAddSmartFilter() const;

    // ---- Smart Filter editing (EditorSessionSmartFilters.cpp): each change one undo step ------------------------
    /// Smart object `id`'s Smart Filters with their shared mask; none when it has none.
    std::optional<nekophoto::SmartFilterStack> smartFilters(const nekophoto::Uuid& id) const;
    /// Whether `id`'s Smart Filters can be changed here (an unlocked smart object whose entries are all drawn here).
    bool canEditSmartFilters(const nekophoto::Uuid& id) const;
    /// `id`'s stack replaced by `stack` (the core re-authors filterFX and the FEid record and redraws), as step `name`.
    bool setSmartFilters(const nekophoto::Uuid& id, const nekophoto::SmartFilterStack& stack, const QString& name, QString* error = nullptr);
    /// Entry `index` (running order, 0 = first applied) replaced by `entry`.
    bool setSmartFilterEntry(const nekophoto::Uuid& id, int index, const nekophoto::SmartFilterEntry& entry, QString* error = nullptr);
    /// Entry `index` switched on or off; -1: the whole stack.
    bool setSmartFilterEnabled(const nekophoto::Uuid& id, int index, bool enabled, QString* error = nullptr);
    /// Entry `from` moved to position `to` (running order).
    bool moveSmartFilter(const nekophoto::Uuid& id, int from, int to, QString* error = nullptr);
    /// Entry `index` deleted (the last one takes the stack with it).
    bool removeSmartFilter(const nekophoto::Uuid& id, int index, QString* error = nullptr);
    /// All of `id`'s Smart Filters removed (Clear Smart Filters).
    bool clearSmartFilters(const nekophoto::Uuid& id, QString* error = nullptr);
    enum class FilterMaskAction { Enable, Disable, Invert, Delete };
    bool smartFilterMask(const nekophoto::Uuid& id, FilterMaskAction action, QString* error = nullptr);
    /// Makes `id`'s shared filter mask the paint target: a temporary layer at the top holds it as its layer mask
    /// (so every mask tool works on it), each edit re-authoring the stack; selecting another layer, saving or
    /// exporting leaves it. `show` also shows the mask on the canvas (Alt-click).
    bool beginFilterMaskEdit(const nekophoto::Uuid& id, bool show = false, QString* error = nullptr);
    bool endFilterMaskEdit();
    /// The smart object whose filter mask is being painted, when it is.
    std::optional<nekophoto::Uuid> filterMaskOwner() const;
    std::optional<nekophoto::Uuid> filterMaskLayer() const { return filterMaskOwner() ? filterMaskLayer_ : std::nullopt; }
    bool filterMaskShown() const { return filterMaskOwner() && filterMaskShown_; }
    void setFilterMaskShown(bool shown);
    /// Leaves Quick Mask and filter-mask editing (their layers are never written).
    void endTemporaryLayers();
    /// The active smart object's contents as a document to edit, with the source they belong to.
    std::optional<std::pair<nekophoto::Document, std::string>> smartObjectContentsForEditing(QString* error) const;
    /// New contents for source `sourceId`, placed in every layer that places it (one undo step).
    bool commitSmartObjectContents(const std::string& sourceId, const nekophoto::Document& contents, QString* error);
    /// A session editing a smart object's contents: where they go back to.
    void setSmartObjectParent(EditorSession* parent, const std::string& sourceId) { smartObjectParent_ = parent; smartObjectSource_ = sourceId; }
    EditorSession* smartObjectParent() const { return smartObjectParent_.data(); }
    const std::string& smartObjectSource() const { return smartObjectSource_; }
    /// Sends this session's document back to the smart object it came from and marks it saved.
    bool commitToSmartObjectParent(QString* error);
    /// Whether a pixel edit on the active layer would replace a smart object's contents with pixels (not on its
    /// mask); `ask` also emits smartObjectPixelsRequested so the window can offer Edit Contents or Rasterize.
    bool smartObjectBlocksPixels(bool ask = false);
    /// Why `tool` cannot work on the active layer here (none when it can): for the tools that paint pixels, a group, an
    /// adjustment layer, a hidden layer, a smart object, or a type, shape or fill layer (a mask takes the paint
    /// instead). The canvas shows the forbidden cursor and the reason from it, and refuses a press for the same.
    std::optional<ToolRefusal> toolRefusal(Tool tool) const;
    /// A press refused for `refusal`: the reason in a message, or the question it asks (smartObjectPixelsRequested,
    /// rasterizeRequested).
    void refuseTool(const ToolRefusal& refusal);
    static Rasterizable rasterizable(const nekophoto::Layer& layer);
    /// The active type, shape or fill layer (solid colour, gradient or pattern) as the plain pixels it shows, at the
    /// document's depth and mode (one undo step), as Photoshop's Rasterize before painting.
    bool rasterizeLayer();

signals:
    void pathsChanged();
    /// The ruler guides changed (added, moved, removed, or by undo); the canvas draws them again.
    void guidesChanged();
    /// The channels, their target or what the canvas shows of them changed.
    void channelsChanged();
    /// The open project changed on disk while there is unsaved work: ask, then call resolveExternalChange.
    void externalChangeConflict(const QString& path);
    /// The open project was reloaded because its package changed on disk.
    void reloadedFromDisk();
    /// A pixel edit was stopped on smart object `id`: offer to edit its contents or rasterize it.
    void smartObjectPixelsRequested(nekophoto::Uuid id);
    /// A canvas press was stopped on type, shape or fill layer `id`: offer to rasterize it (rasterizeLayer).
    void rasterizeRequested(nekophoto::Uuid id);
    /// The document's pixels or structure changed; `region` is the document area affected (empty means all).
    void documentChanged(QRectF region);
    /// The document changed, but the canvas already shows it: a brush stroke committed as its live preview
    /// drew it. Nothing to render again.
    void documentChangedAsShown();
    void layersChanged();
    void selectionChanged();
    void scribblesChanged();
    void quickSelectBusyChanged(bool busy);
    void quickSelectFailed(const QString& error);
    /// A short note for the status bar (the wand's size and next step, why a click did nothing).
    void notice(const QString& text);
    void toolChanged();
    void viewportChanged();
    /// The 32-bit view changed (the canvas renders again).
    void view32Changed();
    void transformChanged();
    void historyChanged();
    void titleChanged();
    void projectPathChanged();
    void error(QString message);
    /// The text editor should open for this layer (a new one, or a text layer clicked with the Text tool).
    void textEditRequested(nekophoto::Uuid id);
    /// The clipping display's overlay came, changed or went (setViewOverlay).
    void viewOverlayChanged();

private:
    std::optional<ViewOverlay> viewOverlay_;
    /// The coverage Select All last made: Select All over it again reuses it, so the document is unchanged and no
    /// history step is added (without comparing pixels).
    std::weak_ptr<const nekophoto::GrayImage> selectAllCoverage_;
    void restore(const nekophoto::DocumentHistory::Snapshot& snapshot);
    void setActiveLayer(const std::optional<nekophoto::Uuid>& id);
    /// The selection as a mask in `layer`'s pixel grid (the selected area white when `revealSelection`, else black);
    /// none without a selection or past the pixel budget.
    std::optional<nekophoto::LayerMask> maskFromSelection(const nekophoto::Layer& layer, bool revealSelection) const;
    void finishDeleting(const std::vector<nekophoto::Uuid>& ids);
    void notifyDocument(QRectF region = {});
    nekophoto::Layer* activeLayerMutable();
    static void adoptClipping(const nekophoto::Uuid& id, std::vector<nekophoto::Layer>& layers);
    static void releaseDetachedClipping(std::vector<nekophoto::Layer>& layers);
    void commitMaskTransform(const TransformEdit& edit);
    void distortLayer16(nekophoto::Layer& layer, const TransformEdit& edit);
    void mergeFloatingTransform16(const TransformEdit& edit);
    void commitDistort(const TransformEdit& edit);
    void mergeFloatingTransform(const TransformEdit& edit);
    void cancelFloatingTransform(const FloatingTransform& floating);
    std::optional<std::pair<nekophoto::LayerTransform, nekophoto::Corners>> distortTarget(const nekophoto::Layer& layer, const TransformEdit& edit) const;
    std::optional<nekophoto::LayerTransform> displayedMaskPlacement(const nekophoto::Layer& layer) const;
    void redrawShape(nekophoto::Layer& layer);
    bool redrawText(nekophoto::Layer& layer);
    bool textEditing_ = false;
    struct TypeEdit {
        nekophoto::Uuid layer;
        bool created = false;
        nekophoto::Document before;
        std::optional<nekophoto::Uuid> previousActive;
    };
    std::optional<TypeEdit> typeEdit_;
    std::optional<nekophoto::Layer> textEditOriginal_;
    QPointer<EditorSession> smartObjectParent_;
    std::string smartObjectSource_;
    void finishDeleting(const std::vector<nekophoto::Uuid>& ids, const std::map<nekophoto::Uuid, nekophoto::Asset>& baked);
    std::optional<nekophoto::Asset> bakeClipping(const nekophoto::Uuid& target) const;
    void clearSelectedPixelsNow(nekophoto::Layer& layer);
    std::unique_ptr<nekophoto::BrushStroke> makeRasterEdit(const nekophoto::Layer& layer, bool mask, const nekophoto::BrushSettings& settings) const;
    void commitRasterEdit(nekophoto::BrushStroke& stroke, const nekophoto::Uuid& layerId, bool mask, const QString& name, QRectF region = {}, bool previewExact = false);

    std::optional<nekophoto::Document> document_;
    nekophoto::DocumentHistory history_;
    std::optional<nekophoto::Uuid> activeLayerId_;
    std::optional<nekophoto::Uuid> previewLayerId_;   // the layer the pixel preview stands in for
    std::set<nekophoto::Uuid> selectedLayerIds_;
    bool isMaskSelected_ = false;
    QString projectPath_;
    std::unique_ptr<DocumentSource> source_;   // never null (a pointer so that this header needs no DocumentSource.h)
    // ---- following the package on disk (EditorSessionWatch.cpp)
    void watchProject();
    void stopWatchingProject();
    void noteExternalChange();
    void checkExternalChange();
    void reloadFromDisk();
    bool reloadProject();
    bool lastStepUndone_ = false;   // the last move through the history was an undo (Toggle Last State)
    QFileSystemWatcher* watcher_ = nullptr;
    QTimer* settle_ = nullptr;
    QByteArray knownDigest_;
    bool asking_ = false;
    int externalReloads_ = 0;
    Tool tool_ = Tool::Move;
    std::optional<TransformEdit> transformEdit_;
    std::unique_ptr<nekophoto::BrushStroke> stroke_;
    std::unique_ptr<nekophoto::MyPaintStroke> myPaint_;   // paints stroke_ when a MyPaint preset is chosen
    std::unique_ptr<nekophoto::TipStroke> tipStroke_;     // stamps into stroke_ when a tip brush is chosen
    nekophoto::BrushSampleTrack sampleTrack_;   // derives each brush sample from the ones before
    /// The pen at `documentPoint` into the stroke: through the stabiliser while smoothing is on (unless `direct`), then
    /// derived and given to the engine painting it.
    void brushTo(QPointF documentPoint, bool direct = false);
    void strokeSample(const nekophoto::BrushSample& raw);
    void takeBrushDirty();
    nekophoto::BrushStabilizer stabilizer_;
    bool stabilizing_ = false;
    std::vector<nekophoto::BrushSample> stabilized_;
    std::optional<nekophoto::BrushSample> lastGiven_;   // the last sample given to the engine, raw
    QTimer stabilizerTick_;       // Stroke Catch-Up: while the pen rests, the stabilised brush closes on it
    QElapsedTimer sincePen_;      // since the last pen event, for the stabiliser's clock between events
    uint32_t strokeSeed_ = 0;
    QTimer myPaintSettle_; // while the pointer rests, a MyPaint brush with slow tracking catches up to it
    nekophoto::Uuid strokeLayerId_;
    QRectF strokeRegion_;   // everything the stroke has painted so far, in document pixels
    bool strokeMask_ = false;
    std::optional<QPointF> lastBrushPoint_;
    /// Copied pixels at the depth of the document they came from.
    /// Pixels copied here: at the source document's depth and layout, with its colour mode and profile, so a paste into a
    /// document of another mode converts them through the profiles (EditorSessionModes.cpp).
    struct PixelClipboard { nekophoto::AnyImage image; QPointF origin; nekophoto::ColorMode mode = nekophoto::ColorMode::RGB; nekophoto::ColorProfile profile; };
    std::optional<PixelClipboard> pixelClipboard_;
    /// The active layer's pixels (or the composite) as they sit on the canvas, inside the selection's whole-pixel bounds.
    std::optional<PixelClipboard> renderSelectedPixels(bool merged) const;
    /// A new layer of `image`, brought to the document's depth.
    void addPixelLayer(nekophoto::AnyImage image, QPointF origin, const QString& editName, bool dropsSelection);
    bool opacityEditing_ = false;
    std::optional<std::map<nekophoto::Uuid, double>> opacityPreview_;   // the layers' opacity before the drag
    bool opacityPreviewFill_ = false;   // the preview is of the layers' Fill
    void setLayerOpacityField(double value, bool fill);
    bool visibilitySwipe_ = false;
    std::map<nekophoto::Uuid, bool> swipeOriginal_;   // each eye the swipe changed, as it was
    const char* swipeName_ = nullptr;
    /// placeLayer's new layer order, or none when the move is not allowed.
    std::optional<std::vector<nekophoto::Layer>> placement(const nekophoto::Uuid& id, const std::optional<nekophoto::Uuid>& parent, const std::optional<nekophoto::Uuid>& above, bool atBottom) const;
    /// Bumped on every document notification; cheap change detection for caches.
    uint64_t documentRevision_ = 0;
    nekophoto::View32 view32_;
    uint64_t peakRevision_ = ~uint64_t(0);
    float peak_ = 0;
    std::optional<nekophoto::AnimationFrame> previewBase_;   // the layer states playback began from
    std::vector<QPointF> strokePoints_;   // the stroke so far, for an action recording it
    std::vector<nekophoto::BrushSample> strokeSamples_;   // the pen at each of those points
    bool timelineEdit(const QString& name, const std::function<bool(nekophoto::Document&)>& change);
    QString importedName_;   // the title of a document that came from an import and has no project path
    std::shared_ptr<const nekophoto::Image> cloneSample_;
    std::shared_ptr<const nekophoto::Image16> cloneSample16_;   // a 16-bit document's
    nekophoto::AnyImage cloneSampleAny_;                        // a 32-bit, CMYK or Lab document's, at its layout
    bool cloneSampleAll_ = false;
    nekophoto::Uuid cloneSampleLayer_;
    uint64_t cloneSampleRevision_ = 0;
    std::shared_ptr<const nekophoto::Image> wandSample_;
    std::vector<Scribble> scribbles_;
    std::vector<ClickPrompt> clickPrompts_;
    std::thread quickSelectThread_;
    bool quickSelectBusy_ = false, quickSelectAgain_ = false;
    /// The flattened document as the wand samples it with Sample All Layers, cached per document revision.
    std::shared_ptr<const nekophoto::Image> flattenedForSampling();
    /// What the click-to-select model sees: flattenedForSampling() in RGB; in CMYK and Lab the composite through the
    /// document's profile to sRGB (the model was trained on sRGB), for deciding only.
    std::shared_ptr<const nekophoto::Image> flattenedForModel();
    bool wandSampleAll_ = false;
    nekophoto::Uuid wandSampleLayer_;
    uint64_t wandSampleRevision_ = 0;
    std::shared_ptr<nekophoto::SmartWandImage> wandSmart_;   // wandSample_ prepared for the edge-aware wand
    /// The latest edge-aware wand selection, while its step is still the latest thing that happened: its
    /// clicks (Shift adds one that selects, Alt one that keeps out), the selection it started from and how it
    /// combines with it. A tolerance change or another click re-evaluates all of them and replaces the step.
    struct WandClick {
        nekophoto::SmartWandImage::Field field;
        int x = 0, y = 0, radius = 0;
        bool positive = true;
        bool anywhere = false;   // Contiguous off: every region that looks like the click
        int tolerance = 32;      // the tolerance this click was made with (later clicks keep theirs, as in Photoshop)
        bool hasStep = false;    // whether this click recorded an undo step (a no-change selection records none)
    };
    struct WandSession {
        std::vector<WandClick> clicks;
        std::optional<nekophoto::Selection> before;
        nekophoto::SelectionMode mode = nekophoto::SelectionMode::Replace;
        uint64_t revisionAfter = 0;
    };
    std::optional<WandSession> wandSession_;
    /// The line colours the last refined wand edge found, and the selection they belong to (Delete uses them
    /// while that selection is unchanged).
    std::vector<uint32_t> wandLineColours_;
    std::shared_ptr<const nekophoto::GrayImage> wandLineSelection_;
    bool wandSessionLive() const;
    /// Thresholds the session's clicks and records the selection: `retune` re-does the latest click at `tolerance` in
    /// place of its step (the Tolerance field moved right after a click); otherwise the latest click is new and gets a
    /// step of its own, so Undo takes back one click at a time.
    void applyWandSession(int tolerance, bool retune);
    bool wandRetuning_ = false;   // the internal undo of a retune is not a user's undo of a wand click
    bool adjustmentEditing_ = false;
    std::optional<nekophoto::Uuid> adjustmentEditLayer_;   // the adjustment layer the open edit has changed
    bool adjustmentTick_ = false;                           // its documentChanged, which keeps documentRevision
    std::optional<nekophoto::Uuid> styleEditLayer_;
    void giveLayerStyle(nekophoto::Layer& layer, const nekophoto::LayerStyle& style);
    std::shared_ptr<const nekophoto::PsdLayerCarry> styleEditCarry_;
    double styleEditFill_ = 1;
    std::shared_ptr<const nekophoto::PsdDocumentCarry> styleEditDocumentCarry_;   // put back on Cancel (patterns the preview added)
    std::optional<nekophoto::LayerStyle> styleClipboard_;
    std::optional<nekophoto::VectorPath::Subpath> penDraft_;
    std::optional<uint16_t> activePathId_;
    std::optional<nekophoto::Selection> lastSelection_;   // what Deselect dropped, for Reselect
    std::optional<int> selectedSubpath_;
    std::optional<nekophoto::Uuid> vectorMaskTarget_;
    /// Where penFinish and finishShape put a new outline: `path` added to the target as a component by `pathOp`,
    /// with `live` properties for it when it is a live shape. False when there is no target to add to.
    bool addComponentToTarget(const nekophoto::VectorPath& path, const std::optional<nekophoto::LiveShape>& live, const QString& name);
    /// The live shape a Shape-tool draft makes (rectangles and ellipses), none for the other kinds.
    std::optional<nekophoto::LiveShape> shapeDraftLive() const;
    std::optional<nekophoto::WarpMesh> warpCage_;
    nekophoto::Uuid warpCageLayer_;
    /// A shape layer bends live (its path redrawn inside the open undo step); this is how it was.
    std::optional<nekophoto::Layer> warpCageShapeBefore_;
    void previewWarpCage();
    bool pathEditing_ = false;
    std::optional<nekophoto::Uuid> quickMaskLayer_, quickMaskReturnLayer_;
    // Channels (EditorSessionChannels.cpp).
    unsigned activeColors_ = nekophoto::colorChannelsAll, visibleColors_ = nekophoto::colorChannelsAll;
    std::optional<nekophoto::ColorMode> channelsModeFor_;
    std::set<nekophoto::Uuid> visibleAlpha_;
    std::optional<nekophoto::Uuid> channelTarget_, channelProxy_, channelReturnLayer_;
    nekophoto::AnyGray channelSynced_;   // the proxy's mask as last written into the channel
    /// The document when the outermost open edit began, while only some colour channels are active.
    std::optional<nekophoto::Document> channelEditBase_;
    int editDepth_ = 0;
    std::optional<nekophoto::Uuid> channelsFor_;   // the document the channel view belongs to
    void followChannelDocument();
    /// The proxy's mask written into its channel when it changed (from endEdit, inside the step).
    void syncChannelProxy();
    /// The proxy made again from the channel (after the channel changed some other way).
    void refreshChannelProxy();
    bool beginChannelEdit(const nekophoto::Uuid& id);
    /// Paste into the active colour channels or the target alpha channel: the clipboard's gray (Photoshop's paste
    /// into a channel). False when it does not apply (the usual paste then runs).
    bool pasteIntoChannels(const nekophoto::AnyImage& image, QPointF origin);
    /// A stroke's preview limited to the active colour channels.
    mutable std::shared_ptr<const nekophoto::Image> channelStrokePreview_;
    mutable std::shared_ptr<const nekophoto::Image16> channelStrokePreview16_;
    std::optional<nekophoto::Uuid> filterMaskLayer_, filterMaskOwner_;
    nekophoto::AnyGray filterMaskSynced_;    // the proxy's mask as last written into the stack
    bool filterMaskShown_ = false;
    mutable std::pair<nekophoto::GrayPtr, nekophoto::ImagePtr> filterMaskView_;   // the mask as gray pixels, cached
    mutable nekophoto::GrayPtr filterMaskWhite_;
    /// The proxy's mask written into its smart object's stack when it changed (from endEdit, inside the step).
    void syncFilterMask();
    /// Starts a stroke that paints `process`'s version of the active layer (as the canvas shows it) through the tip.
    /// `margin`: how far around a pixel `process` reads (it is run a tile at a time with that much around it).
    /// `process16` is the same for a 16-bit document, `processF` for a 32-bit one (none: refused); a Lab document runs
    /// `process` or `process16` on its L, a and b; a CMYK one `processC8` at 8 bits and `process16` on five samples at 16
    /// (none: refused).
    bool beginProcessedStroke(QPointF documentPoint, const std::function<void(nekophoto::Image&)>& process, int margin,
                              const std::function<void(nekophoto::Image16&)>& process16,
                              const std::function<void(nekophoto::ImageF&)>& processF = {},
                              const std::function<void(nekophoto::ImageC8&)>& processC8 = {});
    /// Fills the active layer (or its mask) with `color` through `coverage` (document size; null: everywhere) at
    /// `opacity`, as one undo step named `name`.
    /// With `from` (document size, premultiplied), each pixel takes `from`'s there instead of `color`.
    bool fillThrough(const QColor& color, const nekophoto::GrayImage* coverage, double opacity, const char* name, const nekophoto::Image* from = nullptr);
    /// Fill and Fill Path in a 16-bit document: the colour through `coverage` (null: everywhere) at 16 bits. With
    /// `from` (document size), each pixel takes `from`'s there instead of `color` (the Patch tool).
    bool fillThrough16(const QColor& color, const nekophoto::Gray16* coverage, const char* name, const nekophoto::Image16* from = nullptr);
    nekophoto::AnyImage previewImage_;
    std::optional<nekophoto::LayerTransform> previewTransform_;
    std::optional<std::pair<nekophoto::Uuid, nekophoto::Uuid>> transformDuplicate_; // copy, source
    struct DistortCache {
        nekophoto::Corners corners; nekophoto::LayerTransform transform; nekophoto::ImagePtr source; nekophoto::GrayPtr mask; std::optional<nekophoto::WarpedImage> image; nekophoto::GrayPtr warpedMask;
        // The same for a 16-bit layer.
        nekophoto::Image16Ptr source16; nekophoto::Gray16Ptr mask16; std::optional<nekophoto::WarpedImage16> image16; nekophoto::Gray16Ptr warpedMask16;
    };
    mutable std::map<nekophoto::Uuid, DistortCache> distortCache_;
    struct PixelMove { std::unique_ptr<nekophoto::BrushStroke> raster; nekophoto::Selection origin; bool duplicate; QPointF offset; nekophoto::Uuid layerId; };
    std::unique_ptr<PixelMove> pixelMove_;
    struct GradientEdit { std::unique_ptr<nekophoto::BrushStroke> raster; nekophoto::Uuid layerId; bool mask; QPointF start, end; };
    std::unique_ptr<GradientEdit> gradient_;
    std::optional<ShapeDraft> shapeDraft_;
    std::unique_ptr<nekophoto::WarpStroke> warp_;
    nekophoto::Uuid warpLayerId_;
    nekophoto::LayerTransform warpTransform_;
    std::optional<std::pair<int, qint64>> pendingOpacityDigit_;
    std::optional<nekophoto::BlendMode> blendPreview_;
    QElapsedTimer opacityTimer_;

    // ---- 32 bits (EditorSessionFloat.cpp): the float counterparts of the 16-bit pixel edits.
    bool fillThroughF(const QColor& color, const nekophoto::GrayF* coverage, const char* name);
    void clearSelectedPixelsF(nekophoto::Layer& layer);
    std::optional<PixelClipboard> renderSelectedPixelsF(bool merged, const nekophoto::Rect& region) const;
    void distortLayerF(nekophoto::Layer& layer, const TransformEdit& edit);
    void mergeFloatingTransformF(const TransformEdit& edit);
    std::shared_ptr<nekophoto::GrayF> floatingSelectionF(const TransformEdit& edit) const;
    /// Fills `o` with a 32-bit layer's pending distortion; false when the layer is not one.
    bool distortOverrideF(const nekophoto::Layer& layer, const TransformEdit& edit, nekophoto::LayerOverride& o) const;
    struct DistortCacheF {
        nekophoto::Corners corners; nekophoto::LayerTransform transform; nekophoto::ImageFPtr source; nekophoto::GrayFPtr mask;
        std::optional<nekophoto::WarpedImageF> image; std::shared_ptr<nekophoto::GrayF> warpedMask;
    };
    mutable std::map<nekophoto::Uuid, DistortCacheF> distortCacheF_;

    // ---- CMYK and Lab (EditorSessionModes.cpp): pixels at the document's own layout for the clipboard, Free Transform
    // and Distort; conversions through the profiles between documents of different modes.
    std::optional<PixelClipboard> renderSelectedPixelsNative(bool merged, const nekophoto::Rect& region) const;
    /// What the system clipboard gets from pixels in this document: 8-bit sRGB for CMYK and Lab.
    QImage clipboardImageFor(const nekophoto::AnyImage& image) const;
    /// An asset for pixels at the document's layout, its thumbnail drawn through the document's profile.
    nekophoto::Asset modeAsset(const nekophoto::AnyImage& image, const std::string& name) const;
    void distortLayerAny(nekophoto::Layer& layer, const TransformEdit& edit);
    void mergeFloatingTransformAny(const TransformEdit& edit);
    bool distortOverrideAny(const nekophoto::Layer& layer, const TransformEdit& edit, nekophoto::LayerOverride& o) const;
    struct DistortCacheAny {
        nekophoto::Corners corners; nekophoto::LayerTransform transform; nekophoto::AnyImage source; nekophoto::AnyGray mask;
        std::optional<nekophoto::WarpedAny> image; nekophoto::AnyGray warpedMask;
    };
    mutable std::map<nekophoto::Uuid, DistortCacheAny> distortCacheAny_;
};

} // namespace app
