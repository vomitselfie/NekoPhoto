// Alpha and spot channels (Photoshop's Channels panel), and the colour channels as views: saving and loading
// selections, single-channel editing, and the channel view the canvas shows. The model is `Channel` in
// document.h; see docs/channels.md.
#pragma once
#include "document.h"
#include "selection.h"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

/// The colour channels as bits: red 1, green 2, blue 4. All three are the composite.
constexpr unsigned colorChannelsAll = 7;

// ---- The document's channels ---------------------------------------------------------------------------

const Channel* findChannel(const Document& document, const Uuid& id);
Channel* findChannel(Document& document, const Uuid& id);
int channelIndex(const Document& document, const Uuid& id);
/// "Alpha 1", "Alpha 2", ...: the first `prefix N` no channel has.
std::string nextChannelName(const Document& document, const std::string& prefix = "Alpha");
/// Why one more channel cannot be added (Photoshop's 56 channels in all, or the mask budget), or empty.
std::string channelAddProblem(const Document& document);
/// A new alpha channel of the document's size and depth: black (nothing selected), or `coverage` (either depth,
/// document-sized) as its gray.
Channel makeAlphaChannel(const Document& document, std::string name, const AnyGray& coverage = {});

/// The selection a channel stands for, document-sized at `depth`: its gray while its colour indicates masked
/// areas, the inverse while it indicates selected areas.
AnyGray channelCoverage(const Channel& channel, SampleType depth);
/// Save Selection into an existing channel: the selection (none: nothing selected) combined with what the channel
/// selects in `mode`, as selections combine; the channel keeps its own sense (Color Indicates).
void saveSelectionInto(Channel& channel, const std::optional<Selection>& selection, SelectionMode mode, SampleType depth, int width, int height);
/// Channel Options' Color Indicates: the gray is inverted, so the channel keeps selecting what it did (Photoshop).
void setSelectedAreas(Channel& channel, bool selectedAreas);

/// Where Load Selection (and a Ctrl-click on a thumbnail) takes a selection from.
struct SelectionSource {
    enum Kind { AlphaChannel, Composite, Red, Green, Blue, Transparency, LayerMask };
    Kind kind = AlphaChannel;
    /// The channel (AlphaChannel) or the layer (Transparency, LayerMask).
    Uuid id;
};
/// The source's coverage, document-sized at the document's depth: a channel's selection, the composite's
/// luminosity or one colour channel of it (over white, as Photoshop's channels show transparency), a layer's
/// transparency or its mask. Null with `error` when there is none.
AnyGray selectionSourceCoverage(const Document& document, const SelectionSource& source, std::string* error = nullptr);
/// Load Selection: the source's coverage (inverted with `invert`) combined with the document's selection in `mode`.
/// Nullopt with `error` when the source has none; a result selecting nothing is an empty selection.
std::optional<Selection> loadSelectionFrom(const Document& document, const SelectionSource& source, bool invert, SelectionMode mode,
                                           bool antialiased, std::string* error = nullptr);
/// Photoshop's modifiers on a Ctrl-click of a channel's or a layer's thumbnail: Ctrl replaces the selection,
/// Ctrl+Shift adds to it, Ctrl+Alt subtracts from it, Ctrl+Shift+Alt intersects with it.
SelectionMode thumbnailClickMode(bool shift, bool alt);

// ---- The canvas under the channels ---------------------------------------------------------------------

/// Crop and Canvas Size: the new `width` x `height` canvas's (0, 0) is (x, y) on the old one; the channels
/// follow, black where the old canvas did not reach.
void cropChannels(Document& document, int x, int y, int width, int height);
/// Flip Canvas.
void flipChannels(Document& document, bool horizontal);
/// Image Size: every channel resampled from `fromWidth` x `fromHeight` to the document's size.
void resampleChannels(Document& document, int fromWidth, int fromHeight, Sampling sampling);

// ---- Single-channel editing ----------------------------------------------------------------------------

/// `after` (an edit of a layer's pixels) keeping only the colour channels in `channels` from the edit: the other
/// channels and the alpha stay `before`'s, so transparency never changes. After's pixel (0, 0) lies at (dx, dy) on
/// before's grid, and the result is on before's grid. A null `before` (a blank layer) gives null.
AnyImage keepColorChannels(const AnyImage& before, const AnyImage& after, int dx, int dy, unsigned channels);
/// The same with each raster placed by its transform (a stroke's grown grid over the layer's): null when the two grids
/// do not line up in whole pixels at one scale.
AnyImage keepColorChannels(const AnyImage& before, const LayerTransform& beforeTransform, const AnyImage& after, const LayerTransform& afterTransform,
                           unsigned channels);
/// Every layer whose pixels the edit from `before` to `after` changed, limited to `channels` (keepColorChannels)
/// and put back on its old grid. Layers added, removed, resampled or turned by the edit are left as it made them.
/// True when anything changed.
bool restrictToColorChannels(const Document& before, Document& after, unsigned channels);

// ---- The view ------------------------------------------------------------------------------------------

/// A channel shown over the image: its dark areas tinted with `color` at `opacity`.
struct ChannelOverlay {
    AnyGray image;
    std::array<double, 3> color{1, 0, 0};
    double opacity = 0.5;
};
/// What the canvas shows of the channels: the visible colour channels (one alone in gray, several in their
/// colours), or with none visible a channel in gray; then the overlays.
struct ChannelView {
    unsigned color = colorChannelsAll;
    /// With no colour channel visible: the channel shown in gray (none: black).
    AnyGray gray;
    /// `gray` holds the inverse of the channel (the editing layer's mask).
    bool grayInverted = false;
    std::vector<ChannelOverlay> overlays;
    bool isDefault() const { return color == colorChannelsAll && overlays.empty(); }
};
/// The view applied to a frame `render()` drew: `out` shows `region` at `scale`.
void applyChannelView(Image& out, const Rect& region, double scale, const ChannelView& view);

/// A channel's gray reduced to at most `maxSide` a side, at 8 bits (thumbnails).
std::shared_ptr<GrayImage> channelThumbnail(const AnyGray& image, int maxSide);

} // namespace compositor
