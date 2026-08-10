#include "WaylandDisplayTracker.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <iostream>

#include "Util.hpp"

const struct wl_registry_listener WaylandDisplayTracker::registryListener = {
    .global = &WaylandDisplayTracker::handleGlobal,
    .global_remove = &WaylandDisplayTracker::handleGlobalRemove,
};

const struct wl_output_listener WaylandDisplayTracker::outputListener = {
    .geometry = &WaylandDisplayTracker::handleGeometry,
    .mode = &WaylandDisplayTracker::handleMode,
    .done = &WaylandDisplayTracker::handleDone,
    .scale = &WaylandDisplayTracker::handleScale,
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
    .name = &WaylandDisplayTracker::handleName,
#endif
#ifdef WL_OUTPUT_DESCRIPTION_SINCE_VERSION
    .description = &WaylandDisplayTracker::handleDescription,
#endif
};

const struct zxdg_output_v1_listener WaylandDisplayTracker::xdgOutputListener = {
    .logical_position = &WaylandDisplayTracker::handleXdgLogicalPosition,
    .logical_size = &WaylandDisplayTracker::handleXdgLogicalSize,
    .done = &WaylandDisplayTracker::handleXdgDone,
    .name = &WaylandDisplayTracker::handleXdgName,
    .description = &WaylandDisplayTracker::handleXdgDescription,
};

WaylandDisplayTracker::WaylandDisplayTracker() {}

WaylandDisplayTracker::~WaylandDisplayTracker() { stop(); }

bool WaylandDisplayTracker::init() {
    display = wl_display_connect(nullptr);
    if (!display) {
        LOG_ERROR << "Failed to connect to Wayland display socket." << std::endl;
        return false;
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, this);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    stopFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stopFd < 0) {
        LOG_ERROR << "Failed to create stop eventfd." << std::endl;
        return false;
    }

    running = true;
    eventThread = std::thread([this]() {
        int waylandFd = wl_display_get_fd(display);
        struct pollfd fds[2];
        fds[0].fd = waylandFd;
        fds[0].events = POLLIN;
        fds[1].fd = stopFd;
        fds[1].events = POLLIN;

        while (running) {
            while (wl_display_prepare_read(display) != 0) {
                wl_display_dispatch_pending(display);
            }
            wl_display_flush(display);

            int ret = poll(fds, 2, -1);
            if (ret < 0) {
                wl_display_cancel_read(display);
                break;
            }

            if (fds[1].revents & POLLIN) {
                wl_display_cancel_read(display);
                break;
            }

            if (fds[0].revents & POLLIN) {
                wl_display_read_events(display);
                wl_display_dispatch_pending(display);
            } else {
                wl_display_cancel_read(display);
            }
        }
    });

    LOG << "Native wl_output tracker initialized with " << outputMap.size() << " output(s)." << std::endl;
    return true;
}

void WaylandDisplayTracker::stop() {
    bool expected = true;
    if (!running.compare_exchange_strong(expected, false)) {
        return;  // Already stopped
    }

    if (stopFd >= 0) {
        uint64_t v = 1;
        write(stopFd, &v, sizeof(v));
    }

    if (eventThread.joinable()) {
        eventThread.join();
    }

    if (stopFd >= 0) {
        close(stopFd);
        stopFd = -1;
    }

    if (display) {
        wl_display_disconnect(display);
        display = nullptr;
    }
}

std::vector<WaylandOutputInfo> WaylandDisplayTracker::getOutputs() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<WaylandOutputInfo> result;
    for (const auto& pair : outputMap) {
        result.push_back(pair.second->info);
    }
    return result;
}

void WaylandDisplayTracker::notifyChange() {
    OutputChangeCallback cbCopy;
    std::vector<WaylandOutputInfo> outputs;
    {
        std::lock_guard<std::mutex> lock(mutex);
        cbCopy = callback;
        for (const auto& pair : outputMap) {
            outputs.push_back(pair.second->info);
        }
    }
    if (cbCopy) {
        cbCopy(outputs);
    }
}

void WaylandDisplayTracker::handleGlobal(void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
    auto* self = static_cast<WaylandDisplayTracker*>(data);
    std::string iface(interface);

    if (iface == "zxdg_output_manager_v1") {
        uint32_t bindVer = std::min(version, 3u);
        self->xdgOutputManager = static_cast<zxdg_output_manager_v1*>(wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, bindVer));
    } else if (iface == "wl_output") {
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
        uint32_t maxVer = 4u;
#else
        uint32_t maxVer = 3u;
#endif
        uint32_t bindVer = std::min(version, maxVer);
        wl_output* output = static_cast<wl_output*>(wl_registry_bind(registry, id, &wl_output_interface, bindVer));

        auto outData = std::make_shared<OutputData>();
        outData->tracker = self;
        outData->id = id;
        outData->output = output;
        outData->info.id = id;
        self->outputMap[id] = outData;

        wl_output_add_listener(output, &outputListener, outData.get());

        if (self->xdgOutputManager) {
            outData->xdgOutput = zxdg_output_manager_v1_get_xdg_output(self->xdgOutputManager, output);
            zxdg_output_v1_add_listener(outData->xdgOutput, &xdgOutputListener, outData.get());
        }
    }
}

void WaylandDisplayTracker::handleGlobalRemove(void* data, struct wl_registry* registry, uint32_t id) {
    auto* self = static_cast<WaylandDisplayTracker*>(data);
    bool erased = false;

    {
        std::lock_guard<std::mutex> lock(self->mutex);
        auto it = self->outputMap.find(id);
        if (it != self->outputMap.end()) {
            if (it->second->xdgOutput) {
                zxdg_output_v1_destroy(it->second->xdgOutput);
            }
            if (it->second->output) {
                wl_output_destroy(it->second->output);
            }
            self->outputMap.erase(it);
            erased = true;
        }
    }

    if (erased) {
        self->notifyChange();
    }
}
void WaylandDisplayTracker::handleGeometry(void* data, struct wl_output* output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make,
                                           const char* model, int32_t transform) {
    auto* outData = static_cast<OutputData*>(data);
    outData->info.posX = x;
    outData->info.posY = y;
    if (outData->info.name.empty() && make && model) {
        outData->info.name = std::string(make) + " " + std::string(model);
    }
}

void WaylandDisplayTracker::handleMode(void* data, struct wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        auto* outData = static_cast<OutputData*>(data);
        outData->info.modeWidth = width;
        outData->info.modeHeight = height;
    }
}

void WaylandDisplayTracker::handleScale(void* data, struct wl_output* output, int32_t factor) {
    auto* outData = static_cast<OutputData*>(data);
    outData->info.scale = factor > 0 ? factor : 1;
}

void WaylandDisplayTracker::handleName(void* data, struct wl_output* output, const char* name) {
    auto* outData = static_cast<OutputData*>(data);
    if (name) outData->info.name = name;
}

void WaylandDisplayTracker::handleDescription(void* data, struct wl_output* output, const char* description) {
    auto* outData = static_cast<OutputData*>(data);
    if (description && outData->info.name.empty()) outData->info.name = description;
}

void WaylandDisplayTracker::handleDone(void* data, struct wl_output* output) {
    auto* outData = static_cast<OutputData*>(data);

    int32_t scale = outData->info.scale > 0 ? outData->info.scale : 1;

    if (outData->info.logicalWidth <= 0 && outData->info.modeWidth > 0) {
        outData->info.logicalWidth = outData->info.modeWidth / scale;
    }
    if (outData->info.logicalHeight <= 0 && outData->info.modeHeight > 0) {
        outData->info.logicalHeight = outData->info.modeHeight / scale;
    }

    LOG << "wl_output update: id=" << outData->info.id << " name=" << outData->info.name << " pos=(" << outData->info.posX << "," << outData->info.posY << ")"
        << " mode=" << outData->info.modeWidth << "x" << outData->info.modeHeight << " scale=" << scale << " logical=" << outData->info.logicalWidth << "x" << outData->info.logicalHeight << std::endl;

    if (outData->tracker) {
        outData->tracker->notifyChange();
    }
}

void WaylandDisplayTracker::handleXdgLogicalPosition(void* data, struct zxdg_output_v1* zxdg_output_v1, int32_t x, int32_t y) {
    auto* outData = static_cast<OutputData*>(data);
    outData->info.posX = x;
    outData->info.posY = y;
}

void WaylandDisplayTracker::handleXdgLogicalSize(void* data, struct zxdg_output_v1* zxdg_output_v1, int32_t width, int32_t height) {
    auto* outData = static_cast<OutputData*>(data);
    if (width > 0 && height > 0) {
        outData->info.logicalWidth = width;
        outData->info.logicalHeight = height;
    }
}

void WaylandDisplayTracker::handleXdgDone(void* data, struct zxdg_output_v1* zxdg_output_v1) {}

void WaylandDisplayTracker::handleXdgName(void* data, struct zxdg_output_v1* zxdg_output_v1, const char* name) {
    auto* outData = static_cast<OutputData*>(data);
    if (name) outData->info.name = name;
}

void WaylandDisplayTracker::handleXdgDescription(void* data, struct zxdg_output_v1* zxdg_output_v1, const char* description) {}