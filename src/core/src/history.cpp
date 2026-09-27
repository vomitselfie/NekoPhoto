#include "compositor/history.h"
#include "compositor/psd_carry.h"
#include "compositor/smartobject.h"
#include <set>
#include <unordered_set>

namespace compositor {

DocumentHistory::DocumentHistory(int entryLimit, size_t retainedByteLimit)
    : entryLimit_(std::max(0, entryLimit)), retainedByteLimit_(retainedByteLimit) {}

void DocumentHistory::reset() {
    past_.clear();
    future_.clear();
    pending_.reset();
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
    past_.push_back(makeEntry(pendingName_, std::move(before), Snapshot{document, selection, revision_}, pendingRegion_));
    pendingRegion_ = {};
    future_.clear();
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
    Snapshot result = entry.before;
    revision_ = entry.before.revision;
    stepRegion_ = entry.region;
    future_.push_back(std::move(entry));
    trim(result.document);
    return result;
}

std::optional<DocumentHistory::Snapshot> DocumentHistory::redo() {
    if (!canRedo()) return std::nullopt;
    Entry entry = std::move(future_.back());
    future_.pop_back();
    Snapshot result = entry.after;
    revision_ = entry.after.revision;
    stepRegion_ = entry.region;
    past_.push_back(std::move(entry));
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
    Entry merged = makeEntry(name, std::move(past_[k].before), std::move(past_.back().after), past_[k].region);
    // A region is exact only when every step noted one; otherwise the caller works the change out.
    for (size_t i = k + 1; i < past_.size() && !merged.region.isEmpty(); i++)
        merged.region = past_[i].region.isEmpty() ? Rect() : merged.region.unionWith(past_[i].region);
    past_.resize(k);
    past_.push_back(std::move(merged));
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
    if (const PsdDocumentCarry* carry = document.psdCarry.get()) {
        size_t total = 0;
        for (const auto& resource : carry->resources) total += resource.data.size();
        for (const PsdBlock& b : carry->globals) total += b.data.size();
        add(carry, total);
    }
    for (const auto& [id, source] : document.smartObjects) {
        if (!source) continue;
        if (source->bytes) add(source->bytes.get(), source->bytes->size());
        if (source->image) add(source->image.get(), source->image->byteCount());
        if (source->psdElement) add(source->psdElement.get(), source->psdElement->size());
    }
}

} // namespace

DocumentHistory::Entry DocumentHistory::makeEntry(std::string name, Snapshot before, Snapshot after, Rect region) {
    Entry entry{std::move(name), std::move(before), std::move(after), region, {}};
    std::unordered_set<const void*> seen;
    auto add = [&](const void* identity, size_t bytes) { if (seen.insert(identity).second) entry.buffers.emplace_back(identity, bytes); };
    for (const Snapshot* snapshot : {&entry.before, &entry.after})
        if (snapshot->document) heavyBuffers(*snapshot->document, add);
    return entry;
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
