// Frame animation, as Photoshop's Timeline in frame mode: a document may hold frames, each a snapshot of every
// layer's visibility, position and opacity plus how long it shows. The layers themselves (their pixels, masks,
// styles) are shared by all frames; selecting a frame writes its states into the layers, and an edit made while a
// frame is current is kept in that frame (syncCurrentFrame). A layer a frame does not mention (added after the
// frame was made) keeps whatever state it has when that frame is shown.
#pragma once
#include "geometry.h"
#include "uuid.h"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct Document;
class Image;

struct FrameLayerState {
    bool visible = true;
    Point position;       // the layer transform's origin
    double opacity = 1;
    bool operator==(const FrameLayerState&) const = default;
};

struct AnimationFrame {
    int delayMs = 100;    // how long the frame shows; 0 is "no delay" (viewers show it briefly)
    std::map<Uuid, FrameLayerState> layers;
    bool operator==(const AnimationFrame&) const = default;
};

struct Animation {
    std::vector<AnimationFrame> frames;
    int loopCount = 0;    // 0: forever; n: the animation plays n times
    int current = 0;      // the frame the layers show
    bool operator==(const Animation&) const = default;
    bool empty() const { return frames.empty(); }
};

constexpr int maxAnimationFrames = 10000;
constexpr int maxFrameDelayMs = 655350;   // what a GIF's delay can say

/// The layers' current states as a frame.
AnimationFrame captureFrame(const Document& document, int delayMs = 100);
/// Writes the frame's states into the layers it mentions.
void applyFrame(Document& document, const AnimationFrame& frame);
/// A copy of the document with `states` written back. Playback shows frames by writing them into the layers; the
/// states captured when it began (captureFrame) turn what is on screen back into the real document, which is what
/// a save or an autosave during playback writes.
Document withFrameStates(const Document& document, const AnimationFrame& states);
/// Keeps the current frame in step with the layers (after an edit). Does nothing without frames.
void syncCurrentFrame(Document& document);
/// Makes `index` current: the current frame is synced first, then `index` is applied. False when out of range.
bool selectFrame(Document& document, int index);
/// Starts an animation from the layers as they are (one frame); does nothing when there are frames already.
void ensureAnimation(Document& document, int delayMs = 100);
/// A copy of the current frame after it, made current.
bool duplicateFrame(Document& document, int index);
/// A new frame after `index` showing what the current one shows, made current.
bool insertFrame(Document& document, int index);
/// Removes a frame (the last one removed clears the animation); the next frame, or the one before, becomes current.
bool deleteFrame(Document& document, int index);
/// Moves a frame to `to`, keeping the same frame current.
bool moveFrame(Document& document, int from, int to);
/// Drops states of layers no longer in the document and clamps the current index and delays (after loading or
/// deleting layers).
void pruneAnimation(Document& document);
/// Shifts every stored position (for crops and canvas resizes, which move every layer).
void offsetAnimation(Document& document, double dx, double dy);
/// Scales every stored position (Image Size).
void scaleAnimation(Document& document, double sx, double sy);
/// The document as frame `index` shows it, flattened; null when out of range.
std::shared_ptr<Image> renderFrame(const Document& document, int index);

/// JSON for the project manifest's "animation" key: {"loopCount", "current", "frames": [{"delay", "layers": {id:
/// {"visible", "x", "y", "opacity"}}}]}.
std::string animationJson(const Animation& animation);
/// Parses what animationJson writes; nullopt when it is not that shape.
std::optional<Animation> parseAnimationJson(const std::string& json);

} // namespace compositor
