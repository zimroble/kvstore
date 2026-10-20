// In-memory string keyspace with millisecond TTLs.
//
// Expiry works the way Redis does it:
//  * lazy: every read checks the key's deadline and deletes it if passed;
//  * active: active_expire_cycle() samples random keys that have a TTL and
//    deletes the expired ones, repeating while more than 25% of a sample was
//    expired, so memory is reclaimed even for keys that are never read again.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kv {

using Clock = std::function<int64_t()>;  // returns milliseconds since epoch

int64_t system_now_ms();

class Store {
public:
    static constexpr int64_t kNoExpiry = -1;

    explicit Store(Clock clock = system_now_ms);

    // Returns nullptr if the key does not exist (or just expired).
    const std::string* get(const std::string& key);
    std::string* get_mut(const std::string& key);

    // Sets the value. If `keep_ttl` is false the key's TTL is replaced by
    // `expire_at_ms` (kNoExpiry clears it).
    void set(const std::string& key, std::string value, int64_t expire_at_ms = kNoExpiry,
             bool keep_ttl = false);

    bool del(const std::string& key);
    bool exists(const std::string& key);

    // Returns false if the key does not exist.
    bool expire_at(const std::string& key, int64_t at_ms);
    bool persist(const std::string& key);

    // -2 if the key does not exist, -1 if it has no TTL, else remaining ms.
    int64_t pttl(const std::string& key);
    // Absolute deadline, or kNoExpiry. Key must exist.
    int64_t expire_time(const std::string& key);

    std::size_t size() const { return map_.size(); }
    std::size_t keys_with_ttl() const { return ttl_keys_.size(); }
    void clear();

    // One Redis-style active-expiry pass. Returns the number of keys removed.
    std::size_t active_expire_cycle(std::size_t sample_size = 20, int max_rounds = 16);

    // Visit every live (non-expired) key.
    void for_each(const std::function<void(const std::string& key, const std::string& value,
                                           int64_t expire_at_ms)>& fn);

    int64_t now() const { return clock_(); }

private:
    struct Entry {
        std::string value;
        int64_t expire_at = kNoExpiry;
        std::size_t ttl_slot = 0;  // index in ttl_keys_ when expire_at != kNoExpiry
    };
    using Map = std::unordered_map<std::string, Entry>;

    Map::iterator find_live(const std::string& key);
    void erase(Map::iterator it);
    void set_ttl(Map::iterator it, int64_t at_ms);
    void clear_ttl(Map::iterator it);

    Clock clock_;
    Map map_;
    // Keys that carry a TTL, for O(1) random sampling. Pointers into map_
    // nodes stay valid across rehashing.
    std::vector<const std::string*> ttl_keys_;
    std::mt19937_64 rng_{0x5eed};
};

}  // namespace kv
