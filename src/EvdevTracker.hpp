#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class EvdevTracker {
   public:
    using PhysicalMouseCallback = std::function<void()>;

    EvdevTracker();
    ~EvdevTracker();

    bool init();
    void stop();

    void setPhysicalMouseCallback(PhysicalMouseCallback cb) {
        std::lock_guard<std::mutex> lock(mutex);
        physicalMouseCallback = cb;
    }

   private:
    int epollFd = -1;
    int stopFd = -1;
    std::thread thread;
    std::atomic<bool> running{false};
    std::vector<int> openFds;
    std::mutex mutex;
    PhysicalMouseCallback physicalMouseCallback;

    void scanInputDevices();
    void notifyPhysicalMouseMotion();
};
