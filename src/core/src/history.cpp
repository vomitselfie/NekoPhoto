#include "compositor/history.h"
#include "compositor/psd_carry.h"
#include "compositor/smartobject.h"
#include <cstring>
#include <set>
#include <unordered_set>

namespace compositor {

DocumentHistory::DocumentHistory(int entryLimit, size_t retainedByteLimit)
    : entryLimit_(std::max(0, entryLimit)), retainedByteLimit_(retainedByteLimit) {}

void DocumentHistory::reset() {
    past_.clear();
    future_.clear();
    pending_.reset();
    tip_.clear();
    depth_ = 0;
    revision_ = nextRevision();
    savedRevision_ = revision_;
}

void DocumentHistory::begin(const std::string& name, const std::optional<Document>& document, const std::optional<Uuid>& selection) {
    if (depth_ == 0) {
        pending_ = Snapshot{document, selection, revision_};
        pendingName_ = name;
        pendingRegion_ = {};
    }
    depth_++;
}

void DocumentHistory::end(const std::optional<Document>& document, const std::optional<Uuid>& selection) {
    if (depth_ <= 0) return;
    depth_--;
    if (depth_ != 0 || !pending_) return;
    Snapshot before = std::move(*pending_);
    pending_.reset();
    // Selecting, navigating, and no-op edits must preserve redo history.
    if (before.document == document) return;
    revision_ = nextRevision();
    // The step before this one resolves its patches against the tip; if the document changed outside a step
    // since then, those buffers are about to be replaced, so that step keeps its buffers whole.
    if (!past_.empty() && !past_.back().patches.empty()) {
        bool diverged = !before.document;
        for (const RegionPatch& patch : past_.back().patches) {
            const SlotBuffer* tip = tipBuffer(patch);
            if (diverged || !tip || tip->identity() != readSlot(*before.document, patch.slot, patch.id).identity()) { diverged = true; break; }
        }
        if (diverged) seal(past_.back());
    }
    past_.push_back(makeEntry(pendingName_, std::move(before), Snapshot{document, selection, revision_}, pendingRegion_,
                              past_.empty() ? nullptr : &past_.back()));
    pendingRegion_ = {};
    future_.clear();
    {
        std::vector<std::pair<const RegionPatch*, const void*>> changed;
        for (const RegionPatch& p : past_.back().patches) if (!p.inherited) changed.emplace_back(&p, p.source);
        if (!changed.empty()) inheritPatched(past_, changed, true);
    }
    setTip(document ? &*document : nullptr);
    trim(document);
}

void DocumentHistory::noteRegion(const Rect& region) {
    if (depth_ <= 0 || region.isEmpty()) return;
    pendingRegion_ = pendingRegion_.isEmpty() ? region : pendingRegion_.unionWith(region);
}

std::optional<DocumentHistory::Snapshot> DocumentHistory::undo() {
    if (!canUndo()) return std::nullopt;
    Entry entry = std::move(past_.back());
    past_.pop_back();
    Snapshot result = entry.patches.empty() ? entry.before
        : materialize(entry.before, entry.patches, [&](const RegionPatch& p) { return tipSlot(p); }, true);
    settleOneSided(entry, true);
    revision_ = entry.before.revision;
    stepRegion_ = entry.region;
    future_.push_back(std::move(entry));
    shareStepped(future_);
    setTip(result.document ? &*result.document : nullptr);
    trim(result.document);
    return result;
}

std::optional<DocumentHistory::Snapshot> DocumentHistory::redo() {
    if (!canRedo()) return std::nullopt;
    Entry entry = std::move(future_.back());
    future_.pop_back();
    Snapshot result = entry.patches.empty() ? entry.after
        : materialize(entry.after, entry.patches, [&](const RegionPatch& p) { return tipSlot(p); }, false);
    settleOneSided(entry, false);
    revision_ = entry.after.revision;
    stepRegion_ = entry.region;
    past_.push_back(std::move(entry));
    shareStepped(past_);
    setTip(result.document ? &*result.document : nullptr);
    trim(result.document);
    return result;
}

std::optional<std::vector<uint64_t>> DocumentHistory::revisionsSince(uint64_t since) const {
    if (since == revision_ && depth_ == 0) return std::vector<uint64_t>{};
    for (size_t k = 0; k < past_.size(); k++) {
        if (past_[k].before.revision != since) continue;
        std::vector<uint64_t> out;
        for (size_t i = k; i < past_.size(); i++) out.push_back(past_[i].after.revision);
        return out;
    }
    return std::nullopt;
}

int DocumentHistory::squash(uint64_t since, const std::string& name) {
    if (depth_ > 0) return 0;
    size_t k = 0;
    while (k < past_.size() && past_[k].before.revision != since) k++;
    const int count = int(past_.size() - k);
    if (count < 2) return count;
    // Patched entries are made whole first: the last one's after state from the tip, then each before state from
    // the one after it, back to the first.
    Snapshot first = std::move(past_[k].before), last = std::move(past_.back().after);
    bool patched = false;
    for (size_t i = k; i < past_.size(); i++) patched = patched || !past_[i].patches.empty();
    if (patched) {
        last = materialize(last, past_.back().patches, [&](const RegionPatch& p) { return tipSlot(p); }, false, true);
        std::optional<Document> cur = last.document;
        for (size_t i = past_.size(); i-- > k;) {
            const Snapshot& before = i == k ? first : past_[i].before;
            Snapshot whole = past_[i].patches.empty() || !cur ? before
                : materialize(before, past_[i].patches, [&](const RegionPatch& p) { return std::optional<SlotBuffer>(readSlot(*cur, p.slot, p.id)); }, true);
            if (i == k) first = std::move(whole);
            else cur = std::move(whole.document);
        }
    }
    const std::optional<Document> tipDocument = last.document;   // the merged entry may patch where its parts did not
    Entry merged = makeEntry(name, std::move(first), std::move(last), past_[k].region, k > 0 ? &past_[k - 1] : nullptr);
    // A region is exact only when every step noted one; otherwise the caller works the change out.
    for (size_t i = k + 1; i < past_.size() && !merged.region.isEmpty(); i++)
        merged.region = past_[i].region.isEmpty() ? Rect() : merged.region.unionWith(past_[i].region);
    past_.resize(k);
    past_.push_back(std::move(merged));
    {
        std::vector<std::pair<const RegionPatch*, const void*>> changed;
        for (const RegionPatch& p : past_.back().patches) if (!p.inherited) changed.emplace_back(&p, p.source);
        if (!changed.empty()) inheritPatched(past_, changed, true);
    }
    setTip(tipDocument ? &*tipDocument : nullptr);
    return count;
}

namespace {

/// Calls `add(identity, bytes)` for every heavy buffer `document` holds, at the depth it is held at (a buffer shared
/// by several layers comes up once for each; the caller keeps each identity once).
template <class Add>
void heavyBuffers(const Document& document, Add&& add) {
    auto image = [&](const auto& any) { if (any.identity()) add(any.identity(), any.byteCount()); };
    for (const Layer& layer : document.layers) {
        if (layer.asset) {
            image(layer.asset->image);
            if (layer.asset->thumbnail) add(layer.asset->thumbnail.get(), layer.asset->thumbnail->byteCount());
        }
        if (layer.mask) {
            image(layer.mask->asset.image);
            if (layer.mask->asset.thumbnail) add(layer.mask->asset.thumbnail.get(), layer.mask->asset.thumbnail->byteCount());
        }
        image(layer.shapeImage);
        image(layer.textImage);
        image(layer.smartImage);
        // What a PSD layer carried: shared between snapshots, so counted by the carry's identity.
        if (const PsdLayerCarry* carry = layer.psdCarry.get()) {
            size_t total = carry->blendingRanges.size() + carry->maskData.size() + carry->endRanges.size() + carry->adjustmentJson.size();
            for (const PsdBlock& b : carry->blocks) total += b.data.size();
            for (const PsdBlock& b : carry->endBlocks) total += b.data.size();
            for (const auto& channel : carry->maskChannels) total += channel.second.size();
            for (const auto& plane : carry->planes) total += plane.data.size();
            add(carry, total);
        }
        // A smart object instance's blocks are held by value: each copy is its own.
        if (layer.smartObject)
            for (const PsdBlock& b : layer.smartObject->psdBlocks) if (!b.data.empty()) add(b.data.data(), b.data.size());
    }
    if (document.selection) image(document.selection->coverage);
    for (const Channel& channel : document.channels) {
        image(channel.image);
        if (const PsdChannelCarry* carry = channel.psdCarry.get()) add(carry, carry->displayInfo.size() + carry->plane16.size());
    }
    if (const PsdDocumentCarry* carry = document.psdCarry.get()) {
        size_t total = 0;
        for (const auto& resource : carry->resources) total += resource.data.size();
        for (const PsdBlock& b : carry->globals) total += b.data.size();
        add(carry, total);
    }
    for (const auto& [id, source] : document.smartObjects) {
        if (!source) continue;
        if (source->bytes) add(source->bytes.get(), source->bytes->size());
        image(source->image);   // at its own depth; a copy converted for the document is counted as the layers' pixels
        if (source->psdElement) add(source->psdElement.get(), source->psdElement->size());
    }
}

} // namespace

namespace {

using Slot = DocumentHistory::RegionPatch::Slot;

/// Calls `f` with the buffer `document` holds in a slot (an `AnyImage` or `AnyGray`, by reference); false when
/// it has no such slot.
template <class D, class F>
bool withSlot(D& document, Slot slot, const Uuid& id, F&& f) {
    switch (slot) {
    case Slot::LayerPixels: { auto* l = document.find(id); if (!l || !l->asset) return false; f(l->asset->image); return true; }
    case Slot::LayerMask: { auto* l = document.find(id); if (!l || !l->mask) return false; f(l->mask->asset.image); return true; }
    case Slot::Channel: for (auto& c : document.channels) if (c.id == id) { f(c.image); return true; } return false;
    case Slot::Selection: if (!document.selection) return false; f(document.selection->coverage); return true;
    }
    return false;
}

/// A buffer's pixel bytes: rows start at `rows`, `stride` bytes apart, `pixel` bytes per pixel.
struct RowView { const uint8_t* rows = nullptr; size_t stride = 0, pixel = 0; int width = 0, height = 0; };

template <class Ptr>
RowView rowViewOf(const Ptr& p) {
    RowView v;
    if (!p || p->width() <= 0 || p->height() <= 0) return v;
    v.width = p->width();
    v.height = p->height();
    v.rows = reinterpret_cast<const uint8_t*>(p->row(0));
    v.pixel = p->byteCount() / (size_t(v.width) * size_t(v.height));
    v.stride = v.height > 1 ? size_t(reinterpret_cast<const uint8_t*>(p->row(1)) - v.rows) : v.pixel * size_t(v.width);
    return v;
}

template <class Any>
RowView rowView(const Any& any) { return any.visit([](const auto& p) { return rowViewOf(p); }); }

/// A copy of `base` with `crop` pasted over the patch's region, at whatever depth `base` is held.
template <class Any>
Any pasted(const Any& base, const DocumentHistory::RegionPatch& patch, const std::vector<uint8_t>& crop) {
    return base.visit([&](const auto& p) -> Any {
        if (!p) return Any();
        using T = std::remove_const_t<typename std::decay_t<decltype(p)>::element_type>;
        auto copy = std::make_shared<T>(*p);
        const RowView view = rowViewOf(copy);
        const size_t rowBytes = size_t(patch.width) * view.pixel;
        if (patch.x >= 0 && patch.y >= 0 && patch.x + patch.width <= view.width && patch.y + patch.height <= view.height
            && crop.size() == rowBytes * size_t(patch.height))
            for (int y = 0; y < patch.height; y++)
                std::memcpy(reinterpret_cast<uint8_t*>(copy->row(patch.y + y)) + size_t(patch.x) * view.pixel, crop.data() + size_t(y) * rowBytes, rowBytes);
        return Any(std::shared_ptr<const T>(std::move(copy)));
    });
}

std::vector<uint8_t> cropOf(const RowView& v, const DocumentHistory::RegionPatch& patch) {
    const size_t rowBytes = size_t(patch.width) * v.pixel;
    std::vector<uint8_t> out(rowBytes * size_t(patch.height));
    for (int y = 0; y < patch.height; y++)
        std::memcpy(out.data() + size_t(y) * rowBytes, v.rows + size_t(patch.y + y) * v.stride + size_t(patch.x) * v.pixel, rowBytes);
    return out;
}

/// Fills `patch` with the region where `before` and `after` differ (row compares, then a bounding box) when the
/// two buffers have the same shape and the region is under half of them.
template <class Any>
bool diffPatch(const Any& before, const Any& after, DocumentHistory::RegionPatch& patch) {
    if (!before || !after || before.identity() == after.identity() || before.sampleType() != after.sampleType()) return false;
    const RowView a = rowView(before), b = rowView(after);
    if (!a.rows || !b.rows || a.width != b.width || a.height != b.height || a.pixel != b.pixel || a.pixel == 0) return false;
    const size_t rowBytes = size_t(a.width) * a.pixel;
    int y0 = a.height, y1 = -1;
    size_t x0 = rowBytes, x1 = 0;   // bytes
    for (int y = 0; y < a.height; y++) {
        const uint8_t* ra = a.rows + size_t(y) * a.stride;
        const uint8_t* rb = b.rows + size_t(y) * b.stride;
        if (std::memcmp(ra, rb, rowBytes) == 0) continue;
        if (y0 > y) y0 = y;
        y1 = y;
        size_t l = 0;
        while (l < x0 && ra[l] == rb[l]) l++;
        x0 = std::min(x0, l);
        size_t r = rowBytes;
        while (r > x1 + 1 && ra[r - 1] == rb[r - 1]) r--;
        x1 = std::max(x1, r - 1);
    }
    if (y1 < 0) patch.x = patch.y = patch.width = patch.height = 0;
    else {
        patch.x = int(x0 / a.pixel);
        patch.width = int(x1 / a.pixel) - patch.x + 1;
        patch.y = y0;
        patch.height = y1 - y0 + 1;
    }
    if (2 * size_t(patch.width) * size_t(patch.height) >= size_t(a.width) * size_t(a.height)) return false;
    patch.before = cropOf(a, patch);
    patch.after = cropOf(b, patch);
    return true;
}

/// Replaces with region patches every buffer an edit changed in under half of it; both snapshots then hold none.
template <class Allow>
void patchSnapshots(Document& before, Document& after, std::vector<DocumentHistory::RegionPatch>& patches, Allow&& allow) {
    auto tryPatch = [&](Slot slot, const Uuid& id, auto& beforeBuffer, auto& afterBuffer) {
        if (!beforeBuffer || !afterBuffer || beforeBuffer.identity() == afterBuffer.identity()) return;   // unchanged: shared
        DocumentHistory::RegionPatch patch;
        patch.slot = slot;
        patch.id = id;
        patch.source = beforeBuffer.identity();
        if (!allow(slot, id, patch.source)) return;
        if (!diffPatch(beforeBuffer, afterBuffer, patch)) return;
        beforeBuffer.reset();
        afterBuffer.reset();
        patches.push_back(std::move(patch));
    };
    // Layers and channels are matched where they stand; a step that also reorders them keeps whole buffers.
    const size_t layers = std::min(before.layers.size(), after.layers.size());
    for (size_t i = 0; i < layers; i++) {
        Layer& b = before.layers[i];
        Layer& a = after.layers[i];
        // Buffers first: most steps change none, and the ids are strings held elsewhere.
        auto pixels = [](const Layer& l) { return l.asset ? l.asset->image.identity() : nullptr; };
        auto mask = [](const Layer& l) { return l.mask ? l.mask->asset.image.identity() : nullptr; };
        if ((pixels(b) == pixels(a) && mask(b) == mask(a)) || b.id != a.id) continue;
        if (b.asset && a.asset) tryPatch(Slot::LayerPixels, a.id, b.asset->image, a.asset->image);
        if (b.mask && a.mask) tryPatch(Slot::LayerMask, a.id, b.mask->asset.image, a.mask->asset.image);
    }
    const size_t channels = std::min(before.channels.size(), after.channels.size());
    for (size_t i = 0; i < channels; i++)
        if (before.channels[i].id == after.channels[i].id)
            tryPatch(Slot::Channel, after.channels[i].id, before.channels[i].image, after.channels[i].image);
    if (before.selection && after.selection) tryPatch(Slot::Selection, Uuid(), before.selection->coverage, after.selection->coverage);
}

} // namespace

DocumentHistory::SlotBuffer DocumentHistory::readSlot(const Document& document, RegionPatch::Slot slot, const Uuid& id) {
    SlotBuffer out;
    withSlot(document, slot, id, [&](const auto& any) {
        if constexpr (std::is_same_v<std::decay_t<decltype(any)>, AnyImage>) out.image = any; else out.gray = any;
    });
    return out;
}

const DocumentHistory::SlotBuffer* DocumentHistory::tipBuffer(const RegionPatch& patch) const {
    for (const TipBuffer& t : tip_) if (t.slot == patch.slot && t.id == patch.id) return &t.buffer;
    return nullptr;
}

void DocumentHistory::setTip(const Document* document) {
    tip_.clear();
    if (!document) return;
    for (const auto* list : {&past_, &future_}) {
        if (list->empty()) continue;
        for (const RegionPatch& patch : list->back().patches)
            if (!tipBuffer(patch)) tip_.push_back({patch.slot, patch.id, readSlot(*document, patch.slot, patch.id)});
    }
}

template <class Base>
DocumentHistory::Snapshot DocumentHistory::materialize(const Snapshot& snapshot, const std::vector<RegionPatch>& patches, Base&& base, bool before, bool same) {
    Snapshot out = snapshot;
    if (!out.document) return out;
    for (const RegionPatch& patch : patches) {
        if (patch.inherited && !patch.takes(before)) continue;   // this side keeps its own buffer, or has no slot
        const std::optional<SlotBuffer> from = base(patch);
        if (!from) continue;
        withSlot(*out.document, patch.slot, patch.id, [&](auto& dst) {
            using Any = std::decay_t<decltype(dst)>;
            const Any* source;
            if constexpr (std::is_same_v<Any, AnyImage>) source = &from->image; else source = &from->gray;
            dst = same || patch.inherited ? *source : pasted(*source, patch, before ? patch.before : patch.after);
        });
    }
    return out;
}

void DocumentHistory::seal(Entry& entry) const {
    if (entry.patches.empty()) return;
    std::vector<RegionPatch> patches = std::move(entry.patches);
    entry.patches.clear();
    auto tip = [&](const RegionPatch& p) { return tipSlot(p); };
    entry.after = materialize(entry.after, patches, tip, false, true);
    entry.before = materialize(entry.before, patches, tip, true);
    countBuffers(entry);
}

bool DocumentHistory::countBuffers(Entry& entry) {
    entry.buffers.clear();
    entry.patchBytes = 0;
    for (const RegionPatch& patch : entry.patches) entry.patchBytes += patch.bytes();
    std::unordered_set<const void*> seen;
    bool fresh = false;   // the after side holds a buffer the before side does not
    for (const Snapshot* snapshot : {&entry.before, &entry.after}) {
        const bool after = snapshot == &entry.after;
        if (snapshot->document) heavyBuffers(*snapshot->document, [&](const void* identity, size_t bytes) {
            if (!seen.insert(identity).second) return;
            entry.buffers.emplace_back(identity, bytes);
            fresh = fresh || after;
        });
    }
    return fresh;
}

DocumentHistory::Entry DocumentHistory::makeEntry(std::string name, Snapshot before, Snapshot after, Rect region, const Entry* previous) {
    Entry entry;
    entry.name = std::move(name);
    entry.before = std::move(before);
    entry.after = std::move(after);
    entry.region = region;
    // Redoing a patched step pastes into the state the step before it left, so that state must be where this
    // step starts: the previous entry holds the same buffer, or leaves the slot to the chain.
    auto chained = [&](Slot slot, const Uuid& id, const void* source) {
        if (!previous) return true;
        if (!previous->after.document) return false;
        bool ok = false;
        withSlot(*previous->after.document, slot, id, [&](const auto& any) { ok = any && any.identity() == source; });
        if (!ok) for (const RegionPatch& p : previous->patches) ok = ok || (p.slot == slot && p.id == id);
        return ok;
    };
    // Only a step that brought in new buffers can have changed pixels; the others (renames, visibility, moves) skip
    // the walk.
    if (countBuffers(entry) && entry.before.document && entry.after.document) {
        patchSnapshots(*entry.before.document, *entry.after.document, entry.patches, chained);
        if (!entry.patches.empty()) countBuffers(entry);
    }
    return entry;
}

void DocumentHistory::inheritPatched(std::vector<Entry>& list, const std::vector<std::pair<const RegionPatch*, const void*>>& changed, bool nearAfter) {
    // `list.back()` is the step that changed each buffer; the entries below it, walking away from it, share it.
    for (const auto& [patch, identity] : changed) {
        if (!identity) continue;
        for (size_t j = list.size() - 1; j-- > 0;) {
            Entry& entry = list[j];
            if (!entry.before.document || !entry.after.document) break;
            // An entry already leaving the slot to the chain on both sides is passed over; one that changed it, or
            // leaves only its near side to the chain, ends the walk.
            const RegionPatch* own = nullptr;
            for (const RegionPatch& p : entry.patches) if (p.slot == patch->slot && p.id == patch->id) own = &p;
            if (own && own->inherited && own->sides == RegionPatch::Sides::Both) continue;
            if (own) break;
            auto holds = [&](const Document& doc) {
                bool same = false;
                withSlot(doc, patch->slot, patch->id, [&](const auto& any) { same = any.identity() == identity; });
                return same;
            };
            Document& near = nearAfter ? *entry.after.document : *entry.before.document;
            Document& far = nearAfter ? *entry.before.document : *entry.after.document;
            if (!holds(near)) break;
            // The far side holds the buffer too (the step did not touch the slot), or holds its own or none (the step
            // made or replaced it): then only the near side can wait on the chain.
            const bool both = holds(far);
            withSlot(near, patch->slot, patch->id, [](auto& any) { any.reset(); });
            if (both) withSlot(far, patch->slot, patch->id, [](auto& any) { any.reset(); });
            RegionPatch link;
            link.inherited = true;
            link.sides = both ? RegionPatch::Sides::Both : nearAfter ? RegionPatch::Sides::After : RegionPatch::Sides::Before;
            link.slot = patch->slot;
            link.id = patch->id;
            entry.patches.push_back(std::move(link));
            countBuffers(entry);
            if (!both) break;
        }
    }
}

void DocumentHistory::settleOneSided(Entry& entry, bool undoing) const {
    const RegionPatch::Sides near = undoing ? RegionPatch::Sides::After : RegionPatch::Sides::Before;
    Snapshot& side = undoing ? entry.after : entry.before;
    bool settled = false;
    for (size_t i = 0; i < entry.patches.size();) {
        const RegionPatch& link = entry.patches[i];
        if (!link.inherited || link.sides != near) { i++; continue; }
        // The document stands at the near side, so the tip holds the buffer; the side keeps it from now on.
        if (const SlotBuffer* tip = tipBuffer(link); tip && side.document)
            withSlot(*side.document, link.slot, link.id, [&](auto& dst) {
                if constexpr (std::is_same_v<std::decay_t<decltype(dst)>, AnyImage>) dst = tip->image; else dst = tip->gray;
            });
        entry.patches.erase(entry.patches.begin() + std::ptrdiff_t(i));
        settled = true;
    }
    if (settled) countBuffers(entry);
}

void DocumentHistory::shareStepped(std::vector<Entry>& list) {
    // The buffers the step moved away from (still the tip's) are held, unchanged, by the entries beyond it.
    std::vector<std::pair<const RegionPatch*, const void*>> changed;
    for (const RegionPatch& p : list.back().patches)
        if (!p.inherited) if (const SlotBuffer* tip = tipBuffer(p)) changed.emplace_back(&p, tip->identity());
    if (!changed.empty()) inheritPatched(list, changed, &list == &past_);
}

int DocumentHistory::patchCount() const {
    int n = 0;
    for (auto* list : {&past_, &future_}) for (const Entry& e : *list) for (const RegionPatch& p : e.patches) n += !p.inherited;
    return n;
}

size_t DocumentHistory::retainedBytes(const std::optional<Document>& current) const {
    // Every heavy buffer the snapshots keep, each once however many layers and snapshots share it (keyed on the
    // buffer's identity); the live document's buffers are seen first and cost nothing.
    std::unordered_set<const void*> seen;
    if (current) heavyBuffers(*current, [&](const void* identity, size_t) { seen.insert(identity); });
    size_t bytes = 0;
    for (auto* list : {&past_, &future_})
        for (const Entry& entry : *list)
            for (const auto& [identity, size] : entry.buffers)
                if (seen.insert(identity).second) bytes += size;
    for (auto* list : {&past_, &future_})
        for (const Entry& entry : *list) bytes += entry.patchBytes;
    return bytes;
}

void DocumentHistory::trim(const std::optional<Document>& current) {
    while (int(past_.size() + future_.size()) > entryLimit_ || retainedBytes(current) > retainedByteLimit_) {
        if (!past_.empty()) past_.erase(past_.begin());
        else if (!future_.empty()) future_.erase(future_.begin());
        else break;
    }
}

} // namespace compositor
