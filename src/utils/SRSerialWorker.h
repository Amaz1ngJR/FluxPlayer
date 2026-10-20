#pragma once
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace FluxPlayer {
// Exactly one outstanding job. The caller applies backpressure before handing
// over another frame; failures are propagated before any later mux operation.
class SRSerialWorker {
    std::mutex mutex_;
    std::condition_variable ready_, finished_;
    std::function<void()> job_;
    std::exception_ptr error_;
    bool busy_ = false, stopping_ = false;
    std::thread thread_;
    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            ready_.wait(lock, [&]{return stopping_ || bool(job_);});
            if (!job_) return;
            auto job=std::move(job_);
            job_={};
            lock.unlock();
            std::exception_ptr error;
            try {job();} catch (...) {error=std::current_exception();}
            // Drop the frame reference before signaling completion.
            job={};
            lock.lock();
            error_=error;busy_=false;
            finished_.notify_all();
        }
    }
public:
    SRSerialWorker():thread_([this]{loop();}){}
    ~SRSerialWorker() {
        {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
        ready_.notify_one();
        if(thread_.joinable())thread_.join();
    }
    SRSerialWorker(const SRSerialWorker&)=delete;
    SRSerialWorker& operator=(const SRSerialWorker&)=delete;
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock,[&]{return !busy_;});
        if(error_)std::rethrow_exception(error_);
    }
    void submit(std::function<void()> job) {
        if(!job)throw std::invalid_argument("Empty SR encoder job");
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock,[&]{return !busy_;});
        if(error_)std::rethrow_exception(error_);
        if(stopping_)throw std::runtime_error("SR encoder worker stopped");
        job_=std::move(job);busy_=true;
        ready_.notify_one();
    }
};
}
