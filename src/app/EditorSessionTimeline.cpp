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
    return timelineEdit(QT_TRANSLATE_NOOP("History", "Create Frame Animation"), [](Document& d) { ensureAnimation(d); return true; });
}

bool EditorSession::timelineFramesFromLayers() {
    return timelineEdit(QT_TRANSLATE_NOOP("History", "Make Frames From Layers"), [](Document& d) {
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
    endFramePreview();
    if (index == document_->animation.current) return true;
    // As in Photoshop, choosing a frame is not an undo step: the layers show it and the history stays. Every entry
    // records the current frame with the layers, so undoing an edit still returns to the frame it was made in.
    if (!selectFrame(*document_, index)) return false;
    notifyDocument();
    return true;
}

bool EditorSession::timelineDuplicateFrame() {
    return timelineEdit(QT_TRANSLATE_NOOP("History", "New Frame"), [](Document& d) {
        if (d.animation.empty()) { ensureAnimation(d); }
        return duplicateFrame(d, d.animation.current);
    });
}

bool EditorSession::timelineDeleteFrame(int index) {
    return timelineEdit(QT_TRANSLATE_NOOP("History", "Delete Frame"), [index](Document& d) { return deleteFrame(d, index); });
}

bool EditorSession::timelineMoveFrame(int from, int to) {
    return timelineEdit(QT_TRANSLATE_NOOP("History", "Move Frame"), [from, to](Document& d) { return from != to && moveFrame(d, from, to); });
}

bool EditorSession::timelineSetDelay(int index, int delayMs) {
    if (delayMs < 0 || delayMs > maxFrameDelayMs) return false;
    return timelineEdit(QT_TRANSLATE_NOOP("History", "Frame Delay"), [index, delayMs](Document& d) {
        auto& frames = d.animation.frames;
        if (index == -1) { if (frames.empty()) return false; for (auto& f : frames) f.delayMs = delayMs; return true; }
        if (index < 0 || index >= int(frames.size()) || frames[size_t(index)].delayMs == delayMs) return false;
        frames[size_t(index)].delayMs = delayMs;
        return true;
    });
}

bool EditorSession::timelineSetLoopCount(int loops) {
    if (loops < 0 || loops > 65535) return false;
    return timelineEdit(QT_TRANSLATE_NOOP("History", "Looping Options"), [loops](Document& d) {
        if (d.animation.empty() || d.animation.loopCount == loops) return false;
        d.animation.loopCount = loops;
        return true;
    });
}

bool EditorSession::timelineClear() {
    return timelineEdit(QT_TRANSLATE_NOOP("History", "Delete Animation"), [](Document& d) {
        if (d.animation.empty()) return false;
        d.animation = Animation{};
        return true;
    });
}

void EditorSession::previewFrame(int index) {
    if (!document_ || index < 0 || index >= int(document_->animation.frames.size())) return;
    // The states the layers had before playback, so saves write them and Stop restores them exactly (a frame
    // need not mention every layer the previewed ones did).
    if (!previewBase_) previewBase_ = captureFrame(*document_);
    applyFrame(*document_, document_->animation.frames[size_t(index)]);
    documentRevision_++;
    emit documentChanged({});
    emit layersChanged();
}

void EditorSession::endFramePreview() {
    if (!previewBase_) return;
    const AnimationFrame base = std::move(*previewBase_);
    previewBase_.reset();
    if (!document_) return;
    applyFrame(*document_, base);
    documentRevision_++;
    emit documentChanged({});
    emit layersChanged();
}

Document EditorSession::documentToSave() const {
    if (!document_) return Document(1, 1);
    return previewBase_ ? withFrameStates(*document_, *previewBase_) : *document_;
}

} // namespace app
