// Undo history: whole-document value snapshots that share image buffers, so a
// layer edit records no pixel copies. A port of Document/DocumentHistory.swift.
// A pixel edit confined to under half of a layer's, mask's, channel's or the selection's buffer keeps only the
// region's before and after crops (`RegionPatch`); the full buffer lives once, in the document as it stands.
#pragma once
#include "document.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

class DocumentHistory {
public:
    struct Snapshot {
        std::optional<Document> document;
        std::optional<Uuid> activeLayerId;
        uint64_t revision = 0;
    };

    explicit DocumentHistory(int entryLimit = 100, size_t retainedByteLimit = size_t(256) * 1024 * 1024);

    bool canUndo() const { return depth_ == 0 && !past_.empty(); }
    bool canRedo() const { return depth_ == 0 && !future_.empty(); }
    std::string undoName() const { return past_.empty() ? "" : past_.back().name; }
    std::string redoName() const { return future_.empty() ? "" : future_.back().name; }
    bool isModified() const { return revision_ != savedRevision_; }
    int undoCount() const { return int(past_.size()); }
    /// Names of the recorded edits, oldest first / next-redo first.
    std::vector<std::string> pastNames() const { std::vector<std::string> n; for (auto& e : past_) n.push_back(e.name); return n; }
    std::vector<std::string> futureNames() const { std::vector<std::string> n; for (auto it = future_.rbegin(); it != future_.rend(); ++it) n.push_back(it->name); return n; }
    void markSaved() { savedRevision_ = revision_; }
    /// Counts as unsaved until the next save (revisions start at 1, so 0 matches none): a recovered document.
    void markUnsaved() { savedRevision_ = 0; }
    void reset();

    /// Nestable: only the outermost begin/end pair records an entry, and only if the document changed.
    void begin(const std::string& name, const std::optional<Document>& document, const std::optional<Uuid>& selection);
    void end(const std::optional<Document>& document, const std::optional<Uuid>& selection);
    bool isEditing() const { return depth_ > 0; }

    std::optional<Snapshot> undo();
    std::optional<Snapshot> redo();

    /// While an edit is open: the part of the canvas it changes, when the edit knows it exactly (a brush
    /// stroke does). Undoing or redoing the step then need only render that part again.
    void noteRegion(const Rect& region);
    /// The region of the step the last undo or redo moved over; empty when that step did not note one.
    const Rect& stepRegion() const { return stepRegion_; }

    /// The revision of the document as it stands; each recorded entry ends at a new one.
    uint64_t revision() const { return revision_; }
    /// The revisions the recorded entries end at, oldest first: identifies entries across undo and redo.
    std::vector<uint64_t> pastRevisions() const { std::vector<uint64_t> r; for (auto& e : past_) r.push_back(e.after.revision); return r; }
    /// The revisions of the entries recorded after the document stood at `since`, oldest first; nullopt when
    /// no entry starts there (undone past it, trimmed, or the document replaced).
    std::optional<std::vector<uint64_t>> revisionsSince(uint64_t since) const;
    /// Merges the entries recorded after the document stood at `since` into one called `name`, which undoes
    /// and redoes them all at once. Returns how many entries it merged (0 or 1 leave the history as it was).
    int squash(uint64_t since, const std::string& name);

    /// Bytes retained only by history, excluding images in the live document.
    size_t retainedBytes(const std::optional<Document>& current) const;
    /// How many buffers the recorded entries keep as region patches rather than whole (for tests).
    int patchCount() const;

    /// The part of one buffer an entry changed, kept as its before and after crops. Both snapshots of the entry
    /// hold no buffer in that slot: undo pastes `before` into a copy of the buffer in the document as it stands
    /// (the entry's after state), redo pastes `after` into a copy of the undone one. An `inherited` slot is one
    /// the entry did not change, whose buffer a later entry patched: both sides take the neighbouring state's.
    struct RegionPatch {
        bool inherited = false;
        const void* source = nullptr;   // the before buffer's identity, while the entry is recorded
        enum class Slot : uint8_t { LayerPixels, LayerMask, Channel, Selection };
        Slot slot = Slot::LayerPixels;
        Uuid id;                  // the layer's or channel's; empty for the selection
        int x = 0, y = 0, width = 0, height = 0;
        std::vector<uint8_t> before, after;   // rows of width * bytes per pixel, top-down
        size_t bytes() const { return before.size() + after.size(); }
    };

private:
    struct Entry {
        std::string name;
        Snapshot before, after;
        Rect region;
        /// Buffers the snapshots leave empty, kept as crops of the changed region.
        std::vector<RegionPatch> patches;
        size_t patchBytes = 0;
        /// The heavy buffers the two snapshots hold (identity, bytes), each once, listed when the entry is made:
        /// snapshots never change, so counting the history's memory need not walk their layers again.
        std::vector<std::pair<const void*, size_t>> buffers;
    };
    /// `previous`: the entry before it, whose after state must be where this one starts for a slot to be patched
    /// (a change made outside any step breaks that chain).
    static Entry makeEntry(std::string name, Snapshot before, Snapshot after, Rect region, const Entry* previous);
    void trim(const std::optional<Document>& current);
    /// Lists the entry's buffers and patch bytes; true when its after side holds a buffer its before side does not.
    static bool countBuffers(Entry& entry);
    /// Entries next to `list.back()` holding, unchanged, a buffer it patched (`changed`: the patch and the
    /// buffer's identity) leave it to the chain.
    static void inheritPatched(std::vector<Entry>& list, const std::vector<std::pair<const RegionPatch*, const void*>>& changed);
    /// After an undo or redo moved `list.back()`: the same for the buffers it stepped away from.
    void shareStepped(std::vector<Entry>& list);
    /// A patched slot's buffer: `image` for layer pixels, `gray` for the others.
    struct SlotBuffer { AnyImage image; AnyGray gray; const void* identity() const { return image ? image.identity() : gray.identity(); } };
    /// The buffers of the document as the last recorded, undone or redone step left it, for the slots the
    /// neighbouring entries patch: what their crops are pasted into.
    struct TipBuffer { RegionPatch::Slot slot; Uuid id; SlotBuffer buffer; };
    const SlotBuffer* tipBuffer(const RegionPatch& patch) const;
    std::optional<SlotBuffer> tipSlot(const RegionPatch& patch) const { auto* b = tipBuffer(patch); return b ? std::optional<SlotBuffer>(*b) : std::nullopt; }
    static SlotBuffer readSlot(const Document& document, RegionPatch::Slot slot, const Uuid& id);
    void setTip(const Document* document);
    /// `snapshot` with its patched slots filled from `base` (the buffers at the entry's other side), pasting the
    /// before or after crops; with `same`, `base` already is that side and its buffers are taken as they are.
    template <class Base>
    static Snapshot materialize(const Snapshot& snapshot, const std::vector<RegionPatch>& patches, Base&& base, bool before, bool same = false);
    /// Makes `entry` whole again (no patches) from the tip buffers, its after state.
    void seal(Entry& entry) const;
    uint64_t nextRevision() { return ++counter_; }

    std::vector<Entry> past_, future_;
    uint64_t counter_ = 1;
    uint64_t revision_ = 1;
    uint64_t savedRevision_ = 1;
    std::optional<Snapshot> pending_;
    std::vector<TipBuffer> tip_;
    std::string pendingName_ = "Edit";
    Rect pendingRegion_, stepRegion_;
    int depth_ = 0;
    int entryLimit_;
    size_t retainedByteLimit_;
};

} // namespace compositor
