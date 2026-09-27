// The contents the renderers draw for gradient and pattern fill layers (they have no pixels of their own), kept
// while the layer's carried PSD data lives: least recently used first out, within a byte budget, like MipCache.
#pragma once
#include "psd_carry.h"
#include <cstddef>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>

namespace compositor {

template <class ImagePtrT>
class FillLayerCache {
public:
    explicit FillLayerCache(size_t budget = size_t(256) << 20) : budget_(budget) {}

    /// The contents drawn for `carry` on a `width` x `height` canvas (null when the layer draws nothing), or nullopt
    /// when none are kept; a hit becomes the most recent.
    std::optional<ImagePtrT> find(const std::shared_ptr<const PsdLayerCarry>& carry, int width, int height) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = index_.find(Key{carry.get(), width, height});
        if (found == index_.end()) return std::nullopt;
        auto it = found->second;
        // The address may have been reused by another carry since this entry was made.
        if (it->carry.lock() != carry) { drop(it); return std::nullopt; }
        entries_.splice(entries_.end(), entries_, it);
        return it->image;
    }

    /// Keeps `image` for `carry`, dropping entries whose layer data is gone and then the least recently used past
    /// the budget (the new one stays even when it alone is larger: it is about to be drawn).
    void insert(const std::shared_ptr<const PsdLayerCarry>& carry, int width, int height, ImagePtrT image) {
        std::lock_guard<std::mutex> lock(mutex_);
        const Key key{carry.get(), width, height};
        if (auto found = index_.find(key); found != index_.end()) drop(found->second);
        // Expired entries are swept whenever the cache has doubled since the last sweep: amortised constant time.
        if (entries_.size() >= 2 * sweptAt_ + 16) {
            for (auto it = entries_.begin(); it != entries_.end();) {
                auto next = std::next(it);
                if (it->carry.expired()) drop(it);
                it = next;
            }
            sweptAt_ = entries_.size();
        }
        const size_t size = image ? image->byteCount() : 0;
        entries_.push_back({carry, key, std::move(image), size});
        index_[key] = std::prev(entries_.end());
        bytes_ += size;
        while (bytes_ > budget_ && entries_.size() > 1) drop(entries_.begin());
    }

    size_t bytes() const { std::lock_guard<std::mutex> lock(mutex_); return bytes_; }
    size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return entries_.size(); }

private:
    struct Key {
        const void* carry;
        int width, height;
        bool operator<(const Key& o) const { return std::tie(carry, width, height) < std::tie(o.carry, o.width, o.height); }
    };
    struct Entry { std::weak_ptr<const PsdLayerCarry> carry; Key key; ImagePtrT image; size_t bytes; };
    using Iterator = typename std::list<Entry>::iterator;
    void drop(Iterator it) {
        bytes_ -= it->bytes;
        index_.erase(it->key);
        entries_.erase(it);
        if (sweptAt_ > entries_.size()) sweptAt_ = entries_.size();
    }

    mutable std::mutex mutex_;
    std::list<Entry> entries_;
    std::map<Key, Iterator> index_;
    size_t bytes_ = 0;
    size_t sweptAt_ = 0;
    size_t budget_;
};

} // namespace compositor
