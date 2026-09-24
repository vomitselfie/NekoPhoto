// Undo history: whole-document value snapshots that share image buffers, so a
// layer edit records no pixel copies. A port of Document/DocumentHistory.swift.
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

    /// Bytes retained only by history, excluding images in the live document.
    size_t retainedBytes(const std::optional<Document>& current) const;

private:
    struct Entry { std::string name; Snapshot before, after; };
    void trim(const std::optional<Document>& current);
    uint64_t nextRevision() { return ++counter_; }

    std::vector<Entry> past_, future_;
    uint64_t counter_ = 1;
    uint64_t revision_ = 1;
    uint64_t savedRevision_ = 1;
    std::optional<Snapshot> pending_;
    std::string pendingName_ = "Edit";
    int depth_ = 0;
    int entryLimit_;
    size_t retainedByteLimit_;
};

} // namespace compositor
