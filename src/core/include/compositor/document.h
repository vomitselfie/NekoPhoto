// The document model: layers, groups, masks, clipping links, adjustments and
// the canvas. A port of the value types in Document/EditorSession.swift and
// friends. Everything here is a plain value; images are shared immutably so
// copying a Document (for undo) costs no pixels.
#pragma once
#include "artboard.h"
#include "animation.h"
#include "colormodes.h"
#include "colorprofile.h"
#include "geometry.h"
#include "psd_carry.h"
#include "smartobject.h"
#include "imaget.h"
#include "transform.h"
#include "uuid.h"
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace compositor {

enum class BlendMode { Normal, Multiply, Screen, Overlay, Darken, Lighten, Difference, ColorDodge, ColorBurn, Hue, Saturation, Color, Luminosity,
                       // Photoshop's others (appended: projects store names, and older ones keep their indices)
                       Dissolve, LinearBurn, DarkerColor, LinearDodge, LighterColor, SoftLight, HardLight, VividLight, LinearLight, PinLight,
                       HardMix, Exclusion, Subtract, Divide };
constexpr int blendModeCount = 27;
/// The modes in Photoshop's menu order and groups (a -1 between groups), for pickers.
const std::vector<int>& blendModeMenuOrder();
const char* blendModeName(BlendMode mode);          // "Color Dodge" etc, as in the manifest
bool parseBlendMode(const std::string& name, BlendMode& out);

/// An imported or painted raster with its 96 px thumbnail (ImportedImage).
struct Asset {
    /// The pixels, at the document's depth; 8-bit code reads them with `image.u8()`.
    AnyImage image;
    ImagePtr thumbnail;
    std::string name;
    static Asset make(ImagePtr image, std::string name);
    /// A 16-bit raster; its thumbnail is reduced to 8 bits.
    static Asset make(Image16Ptr image, std::string name);
    /// An 8-bit CMYK raster; its thumbnail is drawn through the default Working CMYK (refreshModeThumbnails redraws it
    /// in the document's profile).
    static Asset make(ImageC8Ptr image, std::string name);
    /// A 32-bit raster; its thumbnail is tone-mapped at exposure 0 to 8 bits.
    static Asset make(ImageFPtr image, std::string name);
    /// Whichever depth `image` holds.
    static Asset makeAny(const AnyImage& image, std::string name);
};

struct MaskAsset {
    /// At the document's depth; 8-bit code reads it with `image.u8()`. The thumbnail is always 8-bit.
    AnyGray image;
    GrayPtr thumbnail;
    static MaskAsset make(GrayPtr image);
    static MaskAsset make(Gray16Ptr image);
    static MaskAsset make(GrayFPtr image);
    static MaskAsset makeAny(const AnyGray& image);
    static MaskAsset solid(bool revealing);
    /// A one-pixel white or black mask at `type`.
    static MaskAsset solid(bool revealing, SampleType type);
};

/// A layer mask: gray coverage over the layer's pixel grid, or placed on its own (LayerMask).
struct LayerMask {
    MaskAsset asset;
    bool enabled = true;
    std::optional<LayerTransform> placement;
    bool linked = true;
    bool operator==(const LayerMask& o) const {
        return asset.image == o.asset.image && enabled == o.enabled && placement == o.placement && linked == o.linked;
    }
    /// White or black, whichever most of the edge is: what shows beyond a placed mask.
    static uint8_t background(const GrayImage& thumbnail);
    /// Where the mask sits once its layer moves from `from` to `to`.
    std::optional<LayerTransform> placementMovingLayer(const LayerTransform& from, const LayerTransform& to) const;
};

enum class AdjustmentKind { HueSaturation, Levels, Curves, Exposure, GradientMap, Grain,
                            Invert, BrightnessContrast, Posterize, Threshold, BlackWhite, ColorBalance, Vibrance, PhotoFilter, ChannelMixer, SelectiveColor,
                            ColorLookup };
constexpr int adjustmentKindCount = 17;
const char* adjustmentKindName(AdjustmentKind kind);
bool parseAdjustmentKind(const std::string& name, AdjustmentKind& out);

/// An adjustment layer's settings. The full settings are kept as the manifest's
/// JSON so unknown or newer fields round-trip; the fields the renderer uses are
/// decoded on demand (see adjustments.h).
struct LayerAdjustment {
    AdjustmentKind kind = AdjustmentKind::Levels;
    std::string json; // the manifest's "adjustment" object, serialized
    bool operator==(const LayerAdjustment&) const = default;
};

enum class ShapeKind { Rectangle, Ellipse };

struct LayerShapeStyle {
    ShapeKind kind = ShapeKind::Rectangle;
    double red = 0, green = 0, blue = 0;
    double cornerRadius = 0;
    bool operator==(const LayerShapeStyle&) const = default;
};

/// One stretch of a text layer in its own style (Photoshop's style runs).
struct TextRun {
    int length = 0;                    // UTF-16 code units of the layer's text (Qt's and Photoshop's unit)
    std::string fontFamily;
    double fontSize = 48;
    bool bold = false, italic = false;
    int weight = 0;                    // 100 (thin) .. 900 (black), from the face's name; 0: from `bold`
    double red = 0, green = 0, blue = 0;
    double letterSpacing = 0;
    double baselineShift = 0;          // pixels, up
    double leading = 0;                // baseline to baseline for a line holding it (the largest on a line wins); 0: 1.2 x size
    enum class Caps { Normal, Small, All } caps = Caps::Normal;
    bool underline = false, strikethrough = false;
    bool operator==(const TextRun&) const = default;
};

/// Photoshop's Warp Text: a preset style bent over the text's layout box.
struct TextWarp {
    std::string style;                 // "warpArc", "warpFlag", ... (Photoshop's names); empty: none
    double bend = 0;                   // percent, -100..100
    double horizontal = 0, vertical = 0;   // distortion, percent
    bool verticalOrientation = false;  // Photoshop's warpRotate Vrtc
    bool active() const { return !style.empty() && style != "warpNone"; }
    bool operator==(const TextWarp&) const = default;
};

/// A text layer's content and style. Its pixels are an ordinary raster the app renders from these (the
/// core has no font engine), so every consumer of the document, the Mac app included, sees pixels.
struct LayerText {
    std::string text;
    std::string fontFamily;            // empty: the system's default sans-serif
    double fontSize = 48;              // document pixels
    bool bold = false, italic = false;
    double red = 0, green = 0, blue = 0;
    int alignment = 0;                 // 0 left, 1 centre, 2 right (multi-line text)
    double lineSpacing = 1;            // multiple of the font's line height
    double letterSpacing = 0;          // extra pixels between glyphs
    /// Paragraph (box) text: lines wrap at the box's width and what does not fit its height is hidden; the first
    /// baseline sits the first line's cap height below the box's top, as Photoshop sets it. 0: point text.
    double boxWidth = 0, boxHeight = 0;
    TextWarp warp;
    /// Text in more than one style: the runs, in order, covering the text (the fields above then mirror the first
    /// run). Empty: all of it in the style above.
    std::vector<TextRun> runs;
    bool operator==(const LayerText&) const = default;
};

/// The text's length in UTF-16 code units.
int utf16Length(const std::string& utf8);
/// The runs to draw `text` with: its own, fitted to its length (the last one stretched or cut), or one run of its
/// style over all of it.
std::vector<TextRun> textRuns(const LayerText& text);
/// The layer's style as one run (its fields).
TextRun baseTextRun(const LayerText& text);
/// Runs kept in step with an edit that turned `before` into `after` (the changed middle takes the style of the run
/// it starts in).
std::vector<TextRun> adjustTextRuns(const std::vector<TextRun>& runs, const std::string& before, const std::string& after);
/// Adjacent equal runs merged, the plain fields set from the first run, and a single run they say in full dropped.
/// A lone run's leading keeps it unless `leadingIsAuto` (a PSD's reader, which records the automatic leading on
/// every run and hands a single style's leading to the layer's line spacing).
void settleTextRuns(LayerText& text, bool leadingIsAuto = false);
/// `after` (an edit of `before` through its plain fields, its runs untouched) with the edit carried into the runs:
/// the text's change moves the run boundaries, a new size scales every run by the same factor, and any other
/// field changed is given to every run.
LayerText carryTextEdit(const LayerText& before, LayerText after);

/// A change to some of a run's style (Photoshop's Character panel on a selection): each field set is given to the
/// runs it covers, the rest left alone.
struct TextRunPatch {
    std::optional<std::string> fontFamily;
    std::optional<double> fontSize;
    std::optional<bool> bold, italic;          // bold also clears the weight (the face is then chosen by `bold`)
    std::optional<int> weight;                 // 0, or 100..900: bold follows (600 and up)
    std::optional<std::array<double, 3>> color;
    std::optional<double> letterSpacing, baselineShift, leading;
    std::optional<TextRun::Caps> caps;
    std::optional<bool> underline, strikethrough;
    void applyTo(TextRun& run) const;
};
/// `text` with `patch` given to the UTF-16 units [start, start + length) (clamped to the text): the runs split at
/// the range's ends, then settled (settleTextRuns).
void styleTextRange(LayerText& text, int start, int length, const TextRunPatch& patch);

struct Layer {
    Uuid id;
    std::optional<Asset> asset;
    LayerTransform transform;
    std::string name;
    bool visible = true;
    std::optional<Uuid> parentId;
    bool isGroup = false;
    /// A folder's children blend straight into what is below it (Photoshop's Pass Through); false isolates them
    /// and composites the folder's result in its own blend mode, as Photoshop does for every other mode.
    bool passThrough = true;
    double opacity = 1;
    BlendMode blendMode = BlendMode::Normal;
    std::optional<Uuid> maskSourceId;
    std::optional<LayerMask> mask;
    std::optional<LayerAdjustment> adjustment;
    /// A shape layer's style (its pixels are an ordinary raster). `shapeImage` is the raster the shape drew;
    /// once the pixels change the layer is plain pixels again.
    std::optional<LayerShapeStyle> shape;
    AnyImage shapeImage;
    /// A text layer's content and style, with the raster it rendered; once the pixels change the layer is
    /// plain pixels again, as with shapes.
    std::optional<LayerText> text;
    AnyImage textImage;
    /// Manifest fields this build does not understand, kept for the round trip.
    std::string extraJson;
    /// What the PSD this layer came from held that NekoPhoto does not model (psd_carry.h).
    std::shared_ptr<const PsdLayerCarry> psdCarry;
    /// A smart object instance (smartobject.h), with the raster it placed; once the pixels change some other way
    /// the layer is plain pixels again, as with text.
    std::optional<SmartObjectInstance> smartObject;
    AnyImage smartImage;
    /// A folder that is an artboard (artboard.h): its background fills the rectangle and its children are clipped to it.
    std::optional<Artboard> artboard;

    Layer() = default;
    /// A new layer holding `asset`, its top-left at `origin`.
    Layer(Asset asset, Point origin);
    /// A new blank layer (pixels are allocated when painting begins).
    Layer(std::string name, Size blankSize);

    bool operator==(const Layer& o) const;
    Point origin() const { return transform.origin; }
    Size size() const { return transform.size; }
    /// Pixel grid of the layer's raster, or the rounded transform size for a blank layer.
    int pixelWidth() const;
    int pixelHeight() const;
    /// Where the mask's pixels sit: its own placement, else the layer's transform.
    LayerTransform maskTransform() const { return mask && mask->placement ? *mask->placement : transform; }
    /// The shape this layer still is: none once its pixels were edited some other way.
    bool isLiveShape() const { return shape && shapeImage && asset && asset->image == shapeImage; }
    /// The text this layer still is, likewise.
    bool isLiveText() const { return text && textImage && asset && asset->image == textImage; }
    /// The smart object this layer still places.
    bool isLiveSmartObject() const { return smartObject && smartImage && asset && asset->image == smartImage; }
};

/// A selection: document-sized coverage (white = selected) with a flag for antialiased edges.
/// Session-only, never saved. A selection with no coverage at all is an explicit empty selection.
struct Selection {
    /// At the document's depth; 8-bit code reads it with `coverage.u8()`.
    AnyGray coverage;
    bool antialiased = true;
    bool operator==(const Selection& o) const { return coverage == o.coverage && antialiased == o.antialiased; }
    bool isEmpty() const;
    Rect bounds() const;
    /// The nonzero pixel bounds, scanned once per coverage raster and remembered.
    const PixelBounds& pixelBounds() const;

private:
    mutable const void* boundsFor_ = nullptr;
    mutable PixelBounds bounds_;
};

/// Alpha channels are saved selections; spot channels hold a spot colour's ink (carried from a PSD, shown, not edited
/// yet). See docs/channels.md.
enum class ChannelKind { Alpha, Spot };

/// An alpha or spot channel (Photoshop's Channels panel below the colour channels): a document-sized gray at the
/// document's depth, as Photoshop shows it in gray. For an alpha channel whose colour indicates masked areas (the
/// default), white is selected; with `selectedAreas`, black is. The overlay tints the channel's dark areas with
/// `color` at `opacity` (for a spot channel: the ink and its solidity).
struct Channel {
    Uuid id;
    std::string name;
    AnyGray image;
    ChannelKind kind = ChannelKind::Alpha;
    std::array<double, 3> color{1, 0, 0};   // 0..1 sRGB
    double opacity = 0.5;
    bool selectedAreas = false;
    /// What the PSD it came from said about it beyond the model (psd_carry.h).
    std::shared_ptr<const PsdChannelCarry> psdCarry;
    bool operator==(const Channel& o) const {
        return id == o.id && name == o.name && image == o.image && kind == o.kind && color == o.color && opacity == o.opacity
            && selectedAreas == o.selectedAreas && psdCarry == o.psdCarry;
    }
};

/// The outcome of a budget check (Document::canCreate and the rest): which rule a size or an addition breaks,
/// and that rule's limit, so the app can say it in the reader's language.
struct BudgetCheck {
    enum Kind { Ok, Side, Image, Project, Masks, Layers };
    Kind kind = Ok;
    SampleType type = SampleType::U8;
    /// The side in pixels (Side), the pixel budget (Image, Project, Masks) or the layer count (Layers).
    long long limit = 0;
    /// The colour mode the limit is for (a CMYK image holds fewer pixels than an RGB one in the same bytes).
    ColorMode mode = ColorMode::RGB;
    explicit operator bool() const { return kind == Ok; }
    /// In English, for automation and logs.
    std::string message() const;
};

struct Document {
    Uuid id;
    int width = 0;
    int height = 0;
    double resolution = 72;
    /// The depth of every layer, mask and selection (one per document, as in Photoshop). Always U8 until
    /// deeper documents land (docs/high-bit-depth-plan.md, P2).
    SampleType sampleType = SampleType::U8;
    /// Image > Mode: RGB, CMYK or Lab (colormodes.h). Every layer's pixels hold colorModeChannels(colorMode)
    /// samples; masks, the selection and alpha channels are grays in every mode.
    ColorMode colorMode = ColorMode::RGB;
    /// The colour profile the pixels are in (colorprofile.h, colormgmt.h); empty: untagged, treated as sRGB.
    ColorProfile profile;
    /// A 32-bit document's values are linear in `profile` (a linear profile, linearProfile()); this is the profile they
    /// are encoded in at 8 and 16 bits, kept from the document they were converted from (empty: untagged), so going
    /// back restores it exactly. None: the gamma counterpart of `profile` (gammaCounterpart()).
    std::optional<ColorProfile> encodedProfile;
    std::vector<Layer> layers; // bottom to top
    std::optional<Selection> selection;
    std::string extraJson;
    /// The PSD's image resources and global blocks, when the document was opened from one (psd_carry.h).
    std::shared_ptr<const PsdDocumentCarry> psdCarry;
    /// Smart object sources, by id, shared by every layer that places them (and by undo snapshots).
    std::map<std::string, std::shared_ptr<const SmartObjectSource>> smartObjects;
    /// Slices (artboard.h), for Export Slices and the PSD's resource 1050.
    std::vector<Slice> slices;
    /// Frame animation (animation.h); empty for a still document.
    Animation animation;
    /// Alpha and spot channels, in the Channels panel's order (channels.h). They do not render.
    std::vector<Channel> channels;
    /// Photoshop's limit: 56 channels in all, the colour channels included.
    static constexpr int maxChannels = 53;

    Document() = default;
    Document(int width, int height);
    bool operator==(const Document& o) const;
    Size size() const { return {double(width), double(height)}; }
    Rect rect() const { return {0, 0, double(width), double(height)}; }

    const Layer* find(const Uuid& id) const;
    Layer* find(const Uuid& id);
    int indexOf(const Uuid& id) const;

    static bool validDimension(int n) { return n >= 1 && n <= maxImageSide; }
    static constexpr int maxLayers = 10000;
    /// One image, layer, mask or canvas: 100 megapixels, as in Compositor for macOS.
    static constexpr long long pixelBudget = 100000000;
    /// All the layers' pixels together, and separately all the masks': a gigapixel, 4 GB of RGBA, so a stack
    /// of full-size game textures fits. Compositor for macOS stops at pixelBudget in total and cannot open a
    /// larger project (see fitsMacBudget).
    static constexpr long long projectPixelBudget = 1000000000;
    /// The pixels held by every layer's image, and by every mask (alpha and spot channels count as masks).
    long long layerPixels() const;
    long long maskPixels() const;

    /// The budgets are bytes (docs/high-bit-depth-plan.md, section 8): the pixel budgets above are an 8-bit RGBA
    /// document's, and a document of another depth or mode holds as many pixels as fit the same memory: sample size
    /// times channels (a 16-bit RGB image holds half as many, an 8-bit CMYK one four fifths). Masks are one channel
    /// in every mode, so their budget follows the depth alone.
    static constexpr long long imagePixelBudget(SampleType type, ColorMode mode = ColorMode::RGB) {
        return pixelBudget * 4 / ((long long)sampleBytes(type) * colorModeChannels(mode));
    }
    static constexpr long long projectPixelBudgetAt(SampleType type, ColorMode mode = ColorMode::RGB) {
        return projectPixelBudget * 4 / ((long long)sampleBytes(type) * colorModeChannels(mode));
    }
    static constexpr long long maskPixelBudgetAt(SampleType type) { return projectPixelBudgetAt(type); }
    long long imagePixelBudget() const { return imagePixelBudget(sampleType, colorMode); }
    long long projectPixelBudgetAt() const { return projectPixelBudgetAt(sampleType, colorMode); }
    /// The bytes every layer's pixels and every mask take at the document's depth.
    long long layerBytes() const;
    long long maskBytes() const;

    /// The budget rules every path that makes a canvas, a layer or pixels goes through, so the limits agree:
    /// a side of at most maxImageSide, one image within imagePixelBudget(type), all the layers' pixels (and
    /// separately all the masks') within projectPixelBudgetAt(type), and at most maxLayers layers.
    /// A canvas, or one image, `width` x `height` at `type`.
    static BudgetCheck canCreate(int width, int height, SampleType type, ColorMode mode = ColorMode::RGB);
    /// One more `width` x `height` image as a new layer of this document, at its depth.
    BudgetCheck canInsertImage(int width, int height) const;
    /// `count` more layers holding `pixels` of layer pixels and `maskPixels` of mask pixels in all.
    BudgetCheck canAddLayers(long long count, long long pixels = 0, long long maskPixels = 0) const;
    /// A whole document (an import, a loaded project, a resize's result) at its own depth.
    static BudgetCheck withinBudget(const Document& document);
    /// Whether Compositor for macOS can open this project: its loader allows pixelBudget in total.
    bool fitsMacBudget() const {
        // The Mac app is 8-bit RGB only.
        if (sampleType != SampleType::U8 || colorMode != ColorMode::RGB) return false;
        // The Mac app reads projects up to version 7: folders with their own opacity, mode or isolation need 8.
        for (const Layer& l : layers) if (l.isGroup && (l.opacity != 1 || l.blendMode != BlendMode::Normal || !l.passThrough || l.artboard)) return false;
        if (!slices.empty()) return false;
        return layerPixels() <= pixelBudget && maskPixels() <= pixelBudget;
    }
};

/// Layer hierarchy helpers (Document/LayerGroups.swift `LayerHierarchy`).
struct HierarchyEntry { const Layer* layer; int depth; bool visible; };
std::vector<HierarchyEntry> hierarchyEntries(const std::vector<Layer>& layers, bool topFirst = false, const std::set<Uuid>* collapsed = nullptr);
/// Visible, non-group layers in render order (bottom to top).
std::vector<const Layer*> renderLayers(const std::vector<Layer>& layers);
std::set<Uuid> effectiveVisibleIds(const std::vector<Layer>& layers);
std::set<Uuid> descendantIds(const std::vector<Layer>& layers, const Uuid& id);
/// Shifts the layers `ids` by (dx, dy) as one rigid move: pixel layers and folders alike, each mask with them
/// (placed or not, linked or not), a vector mask or shape by way of the transform it follows, and the positions
/// the timeline's frames keep for them. Moving an artboard with its contents uses it.
void translateLayers(Document& document, const std::set<Uuid>& ids, double dx, double dy);
/// Parents must exist and be groups, no cycles, nesting at most 64 deep, groups carry no image.
bool validateHierarchy(const std::vector<Layer>& layers, std::string* error = nullptr);
/// Clipping links: sources exist, are not groups or adjustments, no self links or cycles, chains under 256.
bool validateClipping(const std::vector<Layer>& layers, std::string* error = nullptr);

/// The part of the canvas that may look different between two versions of a document: the bounds of every
/// layer added, removed or changed. The whole canvas when something reaches further (a folder, an adjustment
/// layer, the stacking order, the canvas size); an empty rectangle when nothing visible changed.
Rect changedArea(const Document& before, const Document& after);

/// Image > Mode > 8 Bits/Channel or 16 Bits/Channel: every layer, mask, placed raster and the selection converted
/// to `type` (a 16-bit document widens exactly; going to 8 bits rounds), within the byte budgets. False, with `error`
/// saying why, when the result would not fit (a 16-bit document holds half the pixels of an 8-bit one) or the
/// depth is not supported; the document is then unchanged.
///
/// 32 bits (docs/bit-depth.md, "32 bits"): 8 or 16 bits to 32 linearise every layer's colour through the profile's
/// curve; the profile becomes its linear version and the original is kept in `encodedProfile`. From 32 bits, `toning`
/// (HDR Toning's Exposure and Gamma or Highlight Compression; none or the defaults for the values as they are) is
/// applied to each layer's straight colour before it is encoded; at the defaults a document that came from 8 or 16 bits
/// gets its exact values back. Masks, the selection and channels change scale only. 32 bits needs an RGB document.
struct View32;
bool convertSampleType(Document& document, SampleType type, std::string* error = nullptr, const View32* toning = nullptr);
/// Brings every buffer held at another depth to the document's own depth and mode (a layer imported from an 8-bit file
/// into a 16-bit document, say; an 8-bit CMYK layer into a 16-bit CMYK document), sharing converted buffers as the
/// originals were. Lab's a and b keep their neutral point across depths (128 at 8 bits, 16384 at 16). A colour buffer
/// whose channel count is not the mode's is left as it is: changing mode converts colours, which is Image > Mode's
/// work (colormgmt.h). No budget check. True when anything changed.
bool conformToFormat(Document& document);
/// Why `document` would not fit its budgets at `type` in `mode`, or empty when it would.
std::string formatBudgetProblem(const Document& document, SampleType type, ColorMode mode);
/// The same in the document's own mode.
inline std::string sampleTypeBudgetProblem(const Document& document, SampleType type) { return formatBudgetProblem(document, type, document.colorMode); }

/// The name "Layer N" / "Folder N" not yet used.
std::string nextLayerName(const std::vector<Layer>& layers, const std::string& prefix);

} // namespace compositor
