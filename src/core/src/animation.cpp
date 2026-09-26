// Frame animation (animation.h): frames as per-layer states over one shared set of layers.
#include "compositor/animation.h"
#include "compositor/document.h"
#include "compositor/render.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

using json = nlohmann::json;

namespace compositor {

AnimationFrame captureFrame(const Document& document, int delayMs) {
    AnimationFrame frame;
    frame.delayMs = std::clamp(delayMs, 0, maxFrameDelayMs);
    for (const Layer& l : document.layers) frame.layers[l.id] = {l.visible, l.transform.origin, l.opacity};
    return frame;
}

void applyFrame(Document& document, const AnimationFrame& frame) {
    for (Layer& l : document.layers) {
        auto it = frame.layers.find(l.id);
        if (it == frame.layers.end()) continue;
        const Point shift = it->second.position - l.transform.origin;
        l.visible = it->second.visible;
        l.opacity = std::clamp(it->second.opacity, 0.0, 1.0);
        if (shift.x != 0 || shift.y != 0) {
            // A placed mask travels with its layer when linked, as a move does.
            if (l.mask && l.mask->linked && l.mask->placement) l.mask->placement->origin = l.mask->placement->origin + shift;
            l.transform.origin = it->second.position;
        }
    }
}

void syncCurrentFrame(Document& document) {
    Animation& a = document.animation;
    if (a.frames.empty()) return;
    a.current = std::clamp(a.current, 0, int(a.frames.size()) - 1);
    AnimationFrame& frame = a.frames[size_t(a.current)];
    frame = captureFrame(document, frame.delayMs);
}

bool selectFrame(Document& document, int index) {
    Animation& a = document.animation;
    if (index < 0 || index >= int(a.frames.size())) return false;
    syncCurrentFrame(document);
    a.current = index;
    applyFrame(document, a.frames[size_t(index)]);
    return true;
}

void ensureAnimation(Document& document, int delayMs) {
    if (!document.animation.frames.empty()) return;
    document.animation.frames.push_back(captureFrame(document, delayMs));
    document.animation.current = 0;
}

bool duplicateFrame(Document& document, int index) {
    Animation& a = document.animation;
    if (index < 0 || index >= int(a.frames.size()) || int(a.frames.size()) >= maxAnimationFrames) return false;
    syncCurrentFrame(document);
    AnimationFrame copy = a.frames[size_t(index)];
    a.frames.insert(a.frames.begin() + index + 1, std::move(copy));
    a.current = index + 1;
    applyFrame(document, a.frames[size_t(a.current)]);
    return true;
}

bool insertFrame(Document& document, int index) {
    Animation& a = document.animation;
    if (a.frames.empty()) { ensureAnimation(document); return true; }
    if (index < 0 || index >= int(a.frames.size()) || int(a.frames.size()) >= maxAnimationFrames) return false;
    syncCurrentFrame(document);
    AnimationFrame frame = a.frames[size_t(a.current)];
    frame.delayMs = a.frames[size_t(index)].delayMs;
    a.frames.insert(a.frames.begin() + index + 1, std::move(frame));
    a.current = index + 1;
    return true;
}

bool deleteFrame(Document& document, int index) {
    Animation& a = document.animation;
    if (index < 0 || index >= int(a.frames.size())) return false;
    syncCurrentFrame(document);
    a.frames.erase(a.frames.begin() + index);
    if (a.frames.empty()) { a = Animation{}; return true; }
    a.current = std::min(index, int(a.frames.size()) - 1);
    applyFrame(document, a.frames[size_t(a.current)]);
    return true;
}

bool moveFrame(Document& document, int from, int to) {
    Animation& a = document.animation;
    const int n = int(a.frames.size());
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    if (from == to) return true;
    syncCurrentFrame(document);
    AnimationFrame moved = std::move(a.frames[size_t(from)]);
    a.frames.erase(a.frames.begin() + from);
    a.frames.insert(a.frames.begin() + to, std::move(moved));
    // The frame the layers show stays current wherever it went.
    if (a.current == from) a.current = to;
    else if (from < a.current && to >= a.current) a.current--;
    else if (from > a.current && to <= a.current) a.current++;
    return true;
}

void pruneAnimation(Document& document) {
    Animation& a = document.animation;
    if (a.frames.empty()) { a = Animation{}; return; }
    if (a.frames.size() > size_t(maxAnimationFrames)) a.frames.resize(size_t(maxAnimationFrames));
    std::set<Uuid> ids;
    for (const Layer& l : document.layers) ids.insert(l.id);
    for (AnimationFrame& f : a.frames) {
        f.delayMs = std::clamp(f.delayMs, 0, maxFrameDelayMs);
        for (auto it = f.layers.begin(); it != f.layers.end();) it = ids.count(it->first) ? std::next(it) : f.layers.erase(it);
    }
    a.current = std::clamp(a.current, 0, int(a.frames.size()) - 1);
    a.loopCount = std::clamp(a.loopCount, 0, 65535);
}

void offsetAnimation(Document& document, double dx, double dy) {
    for (AnimationFrame& f : document.animation.frames)
        for (auto& [id, state] : f.layers) state.position = state.position + Point(dx, dy);
}

void scaleAnimation(Document& document, double sx, double sy) {
    for (AnimationFrame& f : document.animation.frames)
        for (auto& [id, state] : f.layers) state.position = Point(state.position.x * sx, state.position.y * sy);
}

std::shared_ptr<Image> renderFrame(const Document& document, int index) {
    if (index < 0 || index >= int(document.animation.frames.size())) return nullptr;
    Document copy = document;
    applyFrame(copy, document.animation.frames[size_t(index)]);
    return renderFlattened(copy);
}

std::string animationJson(const Animation& animation) {
    json j;
    j["loopCount"] = animation.loopCount;
    j["current"] = animation.current;
    j["frames"] = json::array();
    for (const AnimationFrame& f : animation.frames) {
        json layers = json::object();
        for (const auto& [id, s] : f.layers) layers[id] = {{"visible", s.visible}, {"x", s.position.x}, {"y", s.position.y}, {"opacity", s.opacity}};
        j["frames"].push_back({{"delay", f.delayMs}, {"layers", layers}});
    }
    return j.dump();
}

std::optional<Animation> parseAnimationJson(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    Animation a;
    auto integerAt = [](const json& o, const char* key, int fallback) { auto it = o.find(key); return it != o.end() && it->is_number_integer() ? it->get<int>() : fallback; };
    a.loopCount = std::clamp(integerAt(j, "loopCount", 0), 0, 65535);
    a.current = integerAt(j, "current", 0);
    auto frames = j.find("frames");
    if (frames == j.end() || !frames->is_array() || frames->size() > size_t(maxAnimationFrames)) return std::nullopt;
    for (const json& f : *frames) {
        if (!f.is_object()) return std::nullopt;
        AnimationFrame frame;
        frame.delayMs = std::clamp(integerAt(f, "delay", 100), 0, maxFrameDelayMs);
        auto layers = f.find("layers");
        if (layers != f.end()) {
            if (!layers->is_object()) return std::nullopt;
            for (auto& [id, s] : layers->items()) {
                if (!s.is_object()) return std::nullopt;
                FrameLayerState state;
                auto visible = s.find("visible");
                if (visible != s.end() && visible->is_boolean()) state.visible = visible->get<bool>();
                auto number = [&](const char* key, double fallback) { auto it = s.find(key); return it != s.end() && it->is_number() && std::isfinite(it->get<double>()) ? it->get<double>() : fallback; };
                state.position = Point(number("x", 0), number("y", 0));
                state.opacity = std::clamp(number("opacity", 1), 0.0, 1.0);
                frame.layers[id] = state;
            }
        }
        a.frames.push_back(std::move(frame));
    }
    if (a.frames.empty()) return Animation{};
    a.current = std::clamp(a.current, 0, int(a.frames.size()) - 1);
    return a;
}

} // namespace compositor
