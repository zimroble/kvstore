#include "store.h"

#include <chrono>

namespace kv {

int64_t system_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Store::Store(Clock clock) : clock_(std::move(clock)) {}

Store::Map::iterator Store::find_live(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return it;
    if (it->second.expire_at != kNoExpiry && it->second.expire_at <= clock_()) {
        erase(it);
        return map_.end();
    }
    return it;
}

void Store::erase(Map::iterator it) {
    clear_ttl(it);
    map_.erase(it);
}

void Store::set_ttl(Map::iterator it, int64_t at_ms) {
    if (it->second.expire_at == kNoExpiry) {
        it->second.ttl_slot = ttl_keys_.size();
        ttl_keys_.push_back(&it->first);
    }
    it->second.expire_at = at_ms;
}

void Store::clear_ttl(Map::iterator it) {
    if (it->second.expire_at == kNoExpiry) return;
    // Swap-remove from the sampling vector and fix the moved key's slot.
    std::size_t slot = it->second.ttl_slot;
    const std::string* last = ttl_keys_.back();
    ttl_keys_[slot] = last;
    map_.find(*last)->second.ttl_slot = slot;
    ttl_keys_.pop_back();
    it->second.expire_at = kNoExpiry;
}

const std::string* Store::get(const std::string& key) {
    auto it = find_live(key);
    return it == map_.end() ? nullptr : &it->second.value;
}

std::string* Store::get_mut(const std::string& key) {
    auto it = find_live(key);
    return it == map_.end() ? nullptr : &it->second.value;
}

void Store::set(const std::string& key, std::string value, int64_t expire_at_ms, bool keep_ttl) {
    auto it = find_live(key);
    if (it == map_.end()) it = map_.emplace(key, Entry{}).first;
    it->second.value = std::move(value);
    if (keep_ttl) return;
    if (expire_at_ms == kNoExpiry)
        clear_ttl(it);
    else
        set_ttl(it, expire_at_ms);
}

bool Store::del(const std::string& key) {
    auto it = find_live(key);
    if (it == map_.end()) return false;
    erase(it);
    return true;
}

bool Store::exists(const std::string& key) { return find_live(key) != map_.end(); }

bool Store::expire_at(const std::string& key, int64_t at_ms) {
    auto it = find_live(key);
    if (it == map_.end()) return false;
    if (at_ms <= clock_()) {
        erase(it);  // a deadline in the past deletes the key, like Redis
        return true;
    }
    set_ttl(it, at_ms);
    return true;
}

bool Store::persist(const std::string& key) {
    auto it = find_live(key);
    if (it == map_.end() || it->second.expire_at == kNoExpiry) return false;
    clear_ttl(it);
    return true;
}

int64_t Store::pttl(const std::string& key) {
    auto it = find_live(key);
    if (it == map_.end()) return -2;
    if (it->second.expire_at == kNoExpiry) return -1;
    return it->second.expire_at - clock_();
}

int64_t Store::expire_time(const std::string& key) {
    auto it = find_live(key);
    return it == map_.end() ? kNoExpiry : it->second.expire_at;
}

void Store::clear() {
    map_.clear();
    ttl_keys_.clear();
}

std::size_t Store::active_expire_cycle(std::size_t sample_size, int max_rounds) {
    std::size_t removed = 0;
    for (int round = 0; round < max_rounds && !ttl_keys_.empty(); ++round) {
        const int64_t now = clock_();
        std::size_t n = std::min(sample_size, ttl_keys_.size());
        std::size_t expired = 0;
        for (std::size_t i = 0; i < n && !ttl_keys_.empty(); ++i) {
            std::uniform_int_distribution<std::size_t> pick(0, ttl_keys_.size() - 1);
            auto it = map_.find(*ttl_keys_[pick(rng_)]);
            if (it->second.expire_at <= now) {
                erase(it);
                ++expired;
            }
        }
        removed += expired;
        if (expired * 4 <= n) break;  // <= 25% stale: good enough, stop for now
    }
    return removed;
}

void Store::for_each(const std::function<void(const std::string&, const std::string&, int64_t)>& fn) {
    const int64_t now = clock_();
    for (const auto& [key, entry] : map_)
        if (entry.expire_at == kNoExpiry || entry.expire_at > now) fn(key, entry.value, entry.expire_at);
}

}  // namespace kv
