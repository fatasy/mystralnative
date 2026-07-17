#include "mystral/fs/async_file.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <utility>

namespace mystral::fs {
namespace {

struct ReadResult {
    std::vector<uint8_t> data;
    std::string error;
};

void readFileSync(
    const std::string& path,
    const async::JobContext* job,
    ReadResult& result) {
    if (job && job->isCancelled()) return;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        result.error = "Failed to open file: " + path;
        return;
    }

    const std::streampos end = file.tellg();
    if (end < 0) {
        result.error = "Failed to determine file size: " + path;
        return;
    }

    const size_t size = static_cast<size_t>(end);
    file.seekg(0, std::ios::beg);
    if (job && job->isCancelled()) return;

    result.data.resize(size);
    if (size > 0 && !file.read(
            reinterpret_cast<char*>(result.data.data()),
            static_cast<std::streamsize>(size))) {
        result.error = "Failed to read file: " + path;
        result.data.clear();
        return;
    }

    if (job && job->isCancelled()) result.data.clear();
}

}  // namespace

struct AsyncFileReader::Impl {
    bool initialized = false;
};

AsyncFileReader& AsyncFileReader::instance() {
    static AsyncFileReader instance;
    return instance;
}

AsyncFileReader::AsyncFileReader() : impl_(std::make_unique<Impl>()) {}

AsyncFileReader::~AsyncFileReader() {
    shutdown();
}

void AsyncFileReader::init() {
    if (impl_->initialized) return;
    if (!async::getJobSystem().isRunning()) {
        std::cerr << "[AsyncFile] Cannot initialize: JobSystem is not running" << std::endl;
        return;
    }
    impl_->initialized = true;
    std::cout << "[AsyncFile] Initialized with native job system" << std::endl;
}

void AsyncFileReader::shutdown() {
    impl_->initialized = false;
}

bool AsyncFileReader::isReady() const {
    return impl_->initialized;
}

void AsyncFileReader::readFile(
    const std::string& path,
    AsyncFileCallback callback,
    uint64_t generation,
    async::JobPriority priority) {
    if (!callback) return;

    if (!impl_->initialized || !async::getJobSystem().isRunning()) {
        ReadResult result;
        readFileSync(path, nullptr, result);
        callback(std::move(result.data), std::move(result.error));
        return;
    }

    auto result = std::make_shared<ReadResult>();
    auto completionCallback = std::make_shared<AsyncFileCallback>(std::move(callback));
    auto handle = async::getJobSystem().submit(
        priority,
        generation,
        [path, result](const async::JobContext& job) {
            readFileSync(path, &job, *result);
        },
        [result, completionCallback](async::JobStatus status) mutable {
            if (status == async::JobStatus::Cancelled) {
                (*completionCallback)({}, "File read cancelled");
                return;
            }
            if (status == async::JobStatus::Failed && result->error.empty()) {
                result->error = "File read job failed";
            }
            (*completionCallback)(std::move(result->data), std::move(result->error));
        });

    if (!handle) (*completionCallback)({}, "File read queue is full or shutting down");
}

bool AsyncFileReader::processCompletedReads() {
    if (!impl_->initialized) return false;
    return async::getJobSystem().processCompletions() > 0;
}

}  // namespace mystral::fs
