#include "compositor/history.h"
#include "compositor/psd_carry.h"
#include "compositor/smartobject.h"
#include <set>

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
    past_.push_back({pendingName_, std::move(before), Snapshot{document, selection, revision_}, pendingRegion_});
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
    Entry merged{name, std::move(past_[k].before), std::move(past_.back().after), past_[k].region};
    // A region is exact only when every step noted one; otherwise the caller works the change out.
    for (size_t i = k + 1; i < past_.size() && !merged.region.isEmpty(); i++)
        merged.region = past_[i].region.isEmpty() ? Rect() : merged.region.unionWith(past_[i].region);
    past_.resize(k);
    past_.push_back(std::move(merged));
    return count;
}

size_t DocumentHistory::retainedBytes(const std::optional<Document>& current) const {
    std::set<const void*> seen;
    // Every heavy buffer a snapshot keeps, at the depth it is held at, each once however many layers and snapshots
    // share it (keyed on the buffer's identity). The live document's buffers are seen first and cost nothing.
    auto count = [&](const void* identity, size_t size, size_t* bytes) {
        if (identity && seen.insert(identity).second && bytes) *bytes += size;
    };
    auto vector = [&](const auto& v, size_t* bytes) { if (!v.empty()) count(v.data(), v.size() * sizeof(v[0]), bytes); };
    auto blocks = [&](const std::vector<PsdBlock>& list, size_t* bytes) { for (const PsdBlock& b : list) vector(b.data, bytes); };
    auto note = [&](const Layer& layer, size_t* bytes) {
        if (layer.asset) {
            count(layer.asset->image.identity(), layer.asset->image.byteCount(), bytes);
            if (layer.asset->thumbnail) count(layer.asset->thumbnail.get(), layer.asset->thumbnail->byteCount(), bytes);
        }
        if (layer.mask) {
            count(layer.mask->asset.image.identity(), layer.mask->asset.image.byteCount(), bytes);
            if (layer.mask->asset.thumbnail) count(layer.mask->asset.thumbnail.get(), layer.mask->asset.thumbnail->byteCount(), bytes);
        }
        for (const AnyImage* image : {&layer.shapeImage, &layer.textImage, &layer.smartImage}) count(image->identity(), image->byteCount(), bytes);
        // What a PSD layer carried: shared between snapshots, so counted by the carry's identity.
        if (const PsdLayerCarry* carry = layer.psdCarry.get(); carry && seen.insert(carry).second && bytes) {
            size_t total = carry->blendingRanges.size() + carry->maskData.size() + carry->endRanges.size() + carry->adjustmentJson.size();
            for (const PsdBlock& b : carry->blocks) total += b.data.size();
            for (const PsdBlock& b : carry->endBlocks) total += b.data.size();
            for (const auto& channel : carry->maskChannels) total += channel.second.size();
            for (const auto& plane : carry->planes) total += plane.data.size();
            *bytes += total;
        }
        // A smart object instance's blocks are held by value: each copy is its own.
        if (layer.smartObject) blocks(layer.smartObject->psdBlocks, bytes);
    };
    auto noteDocument = [&](const Document& document, size_t* bytes) {
        for (const Layer& layer : document.layers) note(layer, bytes);
        if (document.selection) count(document.selection->coverage.identity(), document.selection->coverage.byteCount(), bytes);
        if (const PsdDocumentCarry* carry = document.psdCarry.get(); carry && seen.insert(carry).second && bytes) {
            size_t total = 0;
            for (const auto& resource : carry->resources) total += resource.data.size();
            for (const PsdBlock& b : carry->globals) total += b.data.size();
            *bytes += total;
        }
        for (const auto& [id, source] : document.smartObjects) {
            if (!source) continue;
            if (source->bytes) count(source->bytes.get(), source->bytes->size(), bytes);
            if (source->image) count(source->image.get(), source->image->byteCount(), bytes);
            if (source->psdElement) count(source->psdElement.get(), source->psdElement->size(), bytes);
        }
    };
    if (current) noteDocument(*current, nullptr);
    size_t bytes = 0;
    for (auto* list : {&past_, &future_})
        for (auto& entry : *list)
            for (auto* snapshot : {&entry.before, &entry.after})
                if (snapshot->document) noteDocument(*snapshot->document, &bytes);
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
