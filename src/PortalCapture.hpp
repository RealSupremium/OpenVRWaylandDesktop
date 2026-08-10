#pragma once

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "VRVulkanOverlay.hpp"

struct CapturedFrame {
    const void* bufferId = nullptr;
    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t drmFormat = 0;
    uint64_t drmModifier = 0x00ffffffffffffffULL;
    bool isDmaBuf = false;
    uint8_t* memoryData = nullptr;
};

class PortalCapture {
   public:
    using FrameCallback = std::function<void(const CapturedFrame&)>;

    PortalCapture();
    ~PortalCapture();

    bool initAndRequestCapture(FrameCallback onFrame, const std::string& existingRestoreToken = "", const std::vector<DrmFormatModifier>& drmModifiers = {});
    bool connectPipeWire(const std::vector<DrmFormatModifier>& drmModifiers = {});
    void stopPipeWire();
    bool reconnectPipeWire(const std::vector<DrmFormatModifier>& drmModifiers = {});
    void stop();
    bool isStreaming() const { return streaming; }

    void setEnableDmaBuf(bool enable) { enableDmaBuf = enable; }

    std::string getRestoreToken() const { return restoreToken; }
    uint32_t getPwNodeId() const { return pwNodeId; }
    int32_t getPositionX() const { return posX; }
    int32_t getPositionY() const { return posY; }
    uint32_t getStreamWidth() const { return streamWidth; }
    uint32_t getStreamHeight() const { return streamHeight; }

   private:
    std::atomic<bool> streaming{false};
    FrameCallback frameCallback;
    bool enableDmaBuf = false;

    GDBusConnection* dbusConn = nullptr;
    std::string sessionHandle;
    std::string restoreToken;
    uint32_t pwNodeId = 0;
    uint64_t pwSerial = 0;
    int32_t posX = 0;
    int32_t posY = 0;
    uint32_t streamWidth = 0;
    uint32_t streamHeight = 0;
    pw_main_loop* pwMainLoop = nullptr;
    pw_context* pwContext = nullptr;
    pw_core* pwCore = nullptr;
    pw_stream* pwStream = nullptr;
    spa_hook streamListener{};
    std::thread pwThread;
    std::vector<std::vector<uint8_t>> podStorage;

    uint32_t currentWidth = 0;
    uint32_t currentHeight = 0;
    uint32_t videoFormat = SPA_VIDEO_FORMAT_BGRx;
    uint64_t currentModifier = 0x00ffffffffffffffULL;

    bool createPortalSession();
    void closePortalSession();
    bool parseStreamMetadata(GVariant* startRes);
    void buildDmaBufFormatPods(size_t& bufIdx, const std::vector<DrmFormatModifier>& drmModifiers, std::vector<const spa_pod*>& params);
    void buildFallbackFormatPod(size_t& bufIdx, std::vector<const spa_pod*>& params);
    void updateStreamBuffersAndMeta();

    static void onProcess(void* data);
    static void onParamChanged(void* data, uint32_t id, const struct spa_pod* param);
    static void onStateChanged(void* data, enum pw_stream_state old_state, enum pw_stream_state state, const char* error);
    void handlePipeWireFrame();
};
