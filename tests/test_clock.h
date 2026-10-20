#pragma once

#include <cstdint>
#include <memory>

#include "store.h"

// Manually advanced clock so TTL tests are deterministic.
struct FakeClock {
    std::shared_ptr<int64_t> now = std::make_shared<int64_t>(1'700'000'000'000);
    kv::Clock fn() const {
        auto p = now;
        return [p] { return *p; };
    }
    void advance(int64_t ms) { *now += ms; }
};
