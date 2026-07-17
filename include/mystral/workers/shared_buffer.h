#pragma once

#include "mystral/js/engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace mystral::workers {

struct SharedBufferStorage {
    SharedBufferStorage(uint64_t bufferId, size_t byteLength);

    uint64_t id = 0;
    size_t size = 0;
    std::unique_ptr<uint8_t[]> bytes;
};

class SharedBufferRegistry {
public:
    std::shared_ptr<SharedBufferStorage> create(size_t size);
    std::shared_ptr<SharedBufferStorage> attach(uint64_t id) const;
    bool release(uint64_t id);
    void clear();
    size_t allocatedBytes() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<SharedBufferStorage>> buffers_;
    uint64_t nextId_ = 1;
};

void installSharedBufferBindings(
    js::Engine* engine,
    const std::shared_ptr<SharedBufferRegistry>& registry);

const char* sharedApiSource();

}  // namespace mystral::workers
