#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "ConfigManager.hpp"
#include "InputManager.hpp"
#include "PortalCapture.hpp"
#include "VRVulkanOverlay.hpp"

class CaptureInstance {
   public:
    explicit CaptureInstance(const MonitorConfig& config);
    ~CaptureInstance();

    bool init();
    void destroy();
    void handleFrame(const CapturedFrame& frame);
    void setEnableDmaBuf(bool enable);

    void startCaptureThread(std::weak_ptr<CaptureInstance> self, std::function<void(CaptureInstance&)> onSuccess, std::function<void(CaptureInstance&)> onFailure);

    MonitorConfig config;
    uint32_t waylandOutputId = 0;
    std::atomic<bool> receivedPortalFrame{false};

    std::unique_ptr<InputManager> inputManager;
    std::unique_ptr<VRVulkanOverlay> overlay;
    std::unique_ptr<PortalCapture> portalCapture;
    std::mutex renderMutex;
};
