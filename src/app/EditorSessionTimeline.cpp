// The Timeline's edits (frame animation, compositor/animation.h): each is one undo step; playback previews frames
// without touching history.
#include "EditorSession.h"
#include "compositor/animation.h"

using namespace compositor;

namespace app {

bool EditorSession::timelineEdit(const QString& name, const std::function<bool(Document&)>& change) {
    if (!document_) return false;
    endFramePreview();
    Document doc = *document_;
    syncCurrentFrame(doc);
    if (!change(doc)) return false;
    beginEdit(name);
    document_ = std::move(doc);
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::timelineCreate() {
    if (document_ && !document_->animation.empty()) return false;
    return timelineEdit("Create Frame Animation", [](Document& d) { ensureAnimation(d); return true; });
}

bool EditorSession::timelineFramesFromLayers() {
    return timelineEdit("Make Frames From Layers", [](Document& d) {
        std::vector<Uuid> top;
        for (const Layer& l : d.layers) if (!l.parentId) top.push_back(l.id);
        if (top.empty() || int(top.size()) > maxAnimationFrames) return false;
        const int delay = d.animation.empty() ? 100 : d.animation.frames[size_t(d.animation.current)].delayMs;
        const int loops = d.animation.loopCount;
        d.animation = Animation{};
        d.animation.loopCount = loops;
        for (const Uuid& shown : top) {
            AnimationFrame frame = captureFrame(d, delay);
            for (const Uuid& id : top) frame.layers[id].visible = id == shown;
            d.animation.frames.push_back(std::move(frame));
        }
        d.animation.current = 0;
        applyFrame(d, d.animation.frames[0]);
        return true;
    });
}

bool EditorSession::timelineSelectFrame(int index) {
    if (!document_ || index < 0 || index >= int(document_->animation.frames.size())) return false;
    if (index == document_->animation.current && !framePreview_) return true;
    return timelineEdit("Select Frame", [index](Document& d) { return selectFrame(d, index); });
}

bool EditorSession::timelineDuplicateFrame() {
    return timelineEdit("New Frame", [](Document& d) {
        if (d.animation.empty()) { ensureAnimation(d); }
        return duplicateFrame(d, d.animation.current);
    });
}

bool EditorSession::timelineDeleteFrame(int index) {
    return timelineEdit("Delete Frame", [index](Document& d) { return deleteFrame(d, index); });
}

bool EditorSession::timelineMoveFrame(int from, int to) {
    return timelineEdit("Move Frame", [from, to](Document& d) { return from != to && moveFrame(d, from, to); });
}

bool EditorSession::timelineSetDelay(int index, int delayMs) {
    if (delayMs < 0 || delayMs > maxFrameDelayMs) return false;
    return timelineEdit("Frame Delay", [index, delayMs](Document& d) {
        auto& frames = d.animation.frames;
        if (index == -1) { if (frames.empty()) return false; for (auto& f : frames) f.delayMs = delayMs; return true; }
        if (index < 0 || index >= int(frames.size()) || frames[size_t(index)].delayMs == delayMs) return false;
        frames[size_t(index)].delayMs = delayMs;
        return true;
    });
}

bool EditorSession::timelineSetLoopCount(int loops) {
    if (loops < 0 || loops > 65535) return false;
    return timelineEdit("Looping Options", [loops](Document& d) {
        if (d.animation.empty() || d.animation.loopCount == loops) return false;
        d.animation.loopCount = loops;
        return true;
    });
}

bool EditorSession::timelineClear() {
    return timelineEdit("Delete Animation", [](Document& d) {
        if (d.animation.empty()) return false;
        d.animation = Animation{};
        return true;
    });
}

void EditorSession::previewFrame(int index) {
    if (!document_ || index < 0 || index >= int(document_->animation.frames.size())) return;
    framePreview_ = true;
    applyFrame(*document_, document_->animation.frames[size_t(index)]);
    documentRevision_++;
    emit documentChanged({});
    emit layersChanged();
}

void EditorSession::endFramePreview() {
    if (!framePreview_) return;
    framePreview_ = false;
    if (!document_ || document_->animation.empty()) return;
    const Animation& a = document_->animation;
    applyFrame(*document_, a.frames[size_t(std::clamp(a.current, 0, int(a.frames.size()) - 1))]);
    documentRevision_++;
    emit documentChanged({});
    emit layersChanged();
}

} // namespace app
