#include "CaptureInstance.hpp"

#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "Util.hpp"

CaptureInstance::CaptureInstance(const MonitorConfig& config) : config(config) {}

CaptureInstance::~CaptureInstance() { destroy(); }

bool CaptureInstance::init() {
    if (config.id.empty()) {
        config.id = "mon_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    inputManager = std::make_unique<InputManager>(config.width, config.height);
    if (!inputManager->init()) {
        LOG_ERROR << "Failed to initialize input manager for " << config.name << std::endl;
        return false;
    }

    overlay = std::make_unique<VRVulkanOverlay>(*inputManager);
    if (!overlay->initVR("openvr_wayland_desktop." + config.id, config.name)) {
        LOG_ERROR << "Failed to initialize OpenVR overlay for " << config.name << std::endl;
        return false;
    }

    overlay->setOverlayHeight(2.0f);
    if (!overlay->initVulkan()) {
        LOG_ERROR << "Failed to initialize Vulkan context for " << config.name << std::endl;
        return false;
    }

    portalCapture = std::make_unique<PortalCapture>();
    portalCapture->setEnableDmaBuf(config.enable_dmabuf);

    return true;
}

void CaptureInstance::destroy() {
    if (portalCapture) {
        portalCapture->stop();
        portalCapture.reset();
    }
    std::lock_guard<std::mutex> lock(renderMutex);
    if (overlay) {
        overlay.reset();
    }
    if (inputManager) {
        inputManager.reset();
    }
}

void CaptureInstance::setEnableDmaBuf(bool enable) {
    config.enable_dmabuf = enable;
    if (portalCapture) {
        portalCapture->setEnableDmaBuf(enable);
        auto drmModifiers = (enable && overlay) ? overlay->getSupportedDrmModifiers() : std::vector<DrmFormatModifier>{};
        portalCapture->reconnectPipeWire(drmModifiers);
        if (!enable && overlay) {
            std::lock_guard<std::mutex> lock(renderMutex);
            overlay->clearDmaBufCache();
        }
    }
}

void CaptureInstance::handleFrame(const CapturedFrame& frame) {
    receivedPortalFrame = true;
    std::lock_guard<std::mutex> lock(renderMutex);
    if (overlay && overlay->isOverlayVisible()) {
        VkImage submitImage = VK_NULL_HANDLE;
        if (frame.isDmaBuf && frame.fd >= 0) {
            // Wait for Wayland compositor GPU rendering / implicit fence on the DMA-BUF
            struct pollfd pfd{};
            pfd.fd = frame.fd;
            pfd.events = POLLIN;
            poll(&pfd, 1, 100);

            struct dma_buf_sync syncStart = {DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START};
            ioctl(frame.fd, DMA_BUF_IOCTL_SYNC, &syncStart);

            submitImage = overlay->importDmaBufFrame(frame.bufferId, frame.fd, frame.width, frame.height, frame.stride, frame.drmFormat, frame.drmModifier);

            struct dma_buf_sync syncEnd = {DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END};
            ioctl(frame.fd, DMA_BUF_IOCTL_SYNC, &syncEnd);
        }
        if (submitImage == VK_NULL_HANDLE && frame.memoryData) {
            overlay->updateFrameData(frame.memoryData, frame.width, frame.height, frame.stride);
            submitImage = overlay->getFallbackImage();
        }
        if (submitImage != VK_NULL_HANDLE) {
            overlay->submitFrame(submitImage, frame.width, frame.height);
        }
    }
}

void CaptureInstance::startCaptureThread(std::weak_ptr<CaptureInstance> self, std::function<void(CaptureInstance&)> onSuccess, std::function<void(CaptureInstance&)> onFailure) {
    auto drmModifiers = (config.enable_dmabuf && overlay) ? overlay->getSupportedDrmModifiers() : std::vector<DrmFormatModifier>{};
    std::thread thread([self, drmModifiers, onSuccess, onFailure]() {
        auto inst = self.lock();
        if (!inst) return;

        std::string tokenToUse = inst->config.restore_token;
        bool success = inst->portalCapture->initAndRequestCapture(
            [self](const CapturedFrame& frame) {
                if (auto inst = self.lock()) {
                    inst->handleFrame(frame);
                }
            },
            tokenToUse, drmModifiers);

        if (success) {
            if (auto inst = self.lock()) {
                std::string newToken = inst->portalCapture->getRestoreToken();
                uint32_t newPwNode = inst->portalCapture->getPwNodeId();
                if (!newToken.empty()) inst->config.restore_token = newToken;
                if (newPwNode != 0) inst->config.node_id = newPwNode;

                if (onSuccess) {
                    onSuccess(*inst);
                }
            }
        } else {
            if (auto inst = self.lock()) {
                LOG << "Capture request failed or canceled for " << inst->config.name << ". Cleaning up monitor." << std::endl;
                inst->destroy();
                if (onFailure) {
                    onFailure(*inst);
                }
            }
        }
    });
    thread.detach();
}
