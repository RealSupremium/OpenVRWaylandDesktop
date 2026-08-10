#pragma once

#include <wayland-client.h>
#include <wayland-xdg-output-unstable-v1-client-protocol.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct WaylandOutputInfo {
    uint32_t id = 0;
    std::string name;
    int32_t posX = 0;
    int32_t posY = 0;
    int32_t modeWidth = 0;   // wl_output mode pixel width
    int32_t modeHeight = 0;  // wl_output mode pixel height
    int32_t logicalWidth = 0;
    int32_t logicalHeight = 0;
    int32_t scale = 1;

    int32_t effectiveWidth() const { return logicalWidth > 0 ? logicalWidth : modeWidth; }
    int32_t effectiveHeight() const { return logicalHeight > 0 ? logicalHeight : modeHeight; }
};

class WaylandDisplayTracker {
   public:
    using OutputChangeCallback = std::function<void(const std::vector<WaylandOutputInfo>& outputs)>;
    WaylandDisplayTracker();
    ~WaylandDisplayTracker();
    bool init();
    void stop();
    void setOutputChangeCallback(OutputChangeCallback cb) {
        std::lock_guard<std::mutex> lock(mutex);
        callback = cb;
    }
    std::vector<WaylandOutputInfo> getOutputs();

   private:
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    zxdg_output_manager_v1* xdgOutputManager = nullptr;
    std::atomic<bool> running{false};
    int stopFd = -1;
    std::thread eventThread;
    std::mutex mutex;
    OutputChangeCallback callback;

    struct OutputData {
        WaylandDisplayTracker* tracker = nullptr;
        uint32_t id = 0;
        wl_output* output = nullptr;
        zxdg_output_v1* xdgOutput = nullptr;
        WaylandOutputInfo info;
    };

    std::unordered_map<uint32_t, std::shared_ptr<OutputData>> outputMap;

    static const struct wl_registry_listener registryListener;
    static const struct wl_output_listener outputListener;
    static const struct zxdg_output_v1_listener xdgOutputListener;

    static void handleGlobal(void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version);
    static void handleGlobalRemove(void* data, struct wl_registry* registry, uint32_t id);

    // wl_output callbacks
    static void handleGeometry(void* data, struct wl_output* output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model,
                               int32_t transform);
    static void handleMode(void* data, struct wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
    static void handleDone(void* data, struct wl_output* output);
    static void handleScale(void* data, struct wl_output* output, int32_t factor);
    static void handleName(void* data, struct wl_output* output, const char* name);
    static void handleDescription(void* data, struct wl_output* output, const char* description);

    // zxdg_output_v1 callbacks
    static void handleXdgLogicalPosition(void* data, struct zxdg_output_v1* zxdg_output_v1, int32_t x, int32_t y);
    static void handleXdgLogicalSize(void* data, struct zxdg_output_v1* zxdg_output_v1, int32_t width, int32_t height);
    static void handleXdgDone(void* data, struct zxdg_output_v1* zxdg_output_v1);
    static void handleXdgName(void* data, struct zxdg_output_v1* zxdg_output_v1, const char* name);
    static void handleXdgDescription(void* data, struct zxdg_output_v1* zxdg_output_v1, const char* description);

    void notifyChange();
};