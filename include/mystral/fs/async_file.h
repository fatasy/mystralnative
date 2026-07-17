#pragma once

/**
 * Async file I/O using the native job system.
 *
 * File reads happen on native worker threads and callbacks are invoked on the
 * main thread when job-system completions are drained.
 *
 * Usage:
 *   fs::readFileAsync("./assets/model.glb", [](std::vector<uint8_t> data, std::string error) {
 *       if (error.empty()) {
 *           // Process data
 *       }
 *   });
 *
 * The callback runs on the main thread during JobSystem::processCompletions().
 */

#include "mystral/async/job_system.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mystral {
namespace fs {

/**
 * Callback type for async file reads.
 * Called on the main thread when the read completes.
 * If error is non-empty, data will be empty.
 */
using AsyncFileCallback = std::function<void(std::vector<uint8_t> data, std::string error)>;

/**
 * Singleton facade for file reads submitted to the native job system.
 */
class AsyncFileReader {
public:
    /**
     * Get the singleton instance.
     */
    static AsyncFileReader& instance();

    /**
     * Initialize the async file reader.
     * Must be called after JobSystem::start().
     */
    void init();

    /**
     * Shutdown and cleanup.
     */
    void shutdown();

    /**
     * Check if the reader is initialized and ready.
     */
    bool isReady() const;

    /**
     * Read a file asynchronously.
     * The callback is invoked on the main thread when complete.
     */
    void readFile(
        const std::string& path,
        AsyncFileCallback callback,
        uint64_t generation = 0,
        async::JobPriority priority = async::JobPriority::Streaming);

    /**
     * Process completed file reads, invoking their callbacks.
     * Call this from the main loop.
     * Returns true if any callbacks were invoked.
     */
    bool processCompletedReads();

    // Prevent copying
    AsyncFileReader(const AsyncFileReader&) = delete;
    AsyncFileReader& operator=(const AsyncFileReader&) = delete;

private:
    AsyncFileReader();
    ~AsyncFileReader();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Convenience function to get the async file reader.
 */
inline AsyncFileReader& getAsyncFileReader() {
    return AsyncFileReader::instance();
}

/**
 * Convenience function to read a file asynchronously.
 */
inline void readFileAsync(
    const std::string& path,
    AsyncFileCallback callback,
    uint64_t generation = 0,
    async::JobPriority priority = async::JobPriority::Streaming) {
    AsyncFileReader::instance().readFile(
        path,
        std::move(callback),
        generation,
        priority);
}

} // namespace fs
} // namespace mystral
