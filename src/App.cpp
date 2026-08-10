#include "App.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "ManifestManager.hpp"
#include "TrayManager.hpp"
#include "Util.hpp"

std::atomic<bool> g_appRunning{true};

void signalHandler(int signum) {
    (void)signum;
    g_appRunning = false;
}

const WaylandOutputInfo* App::matchWaylandOutput(int32_t posX, int32_t posY, uint32_t width, uint32_t height, const std::vector<WaylandOutputInfo>& waylandOutputs) {
    for (const auto& out : waylandOutputs) {
        if (out.posX == posX && out.posY == posY && out.effectiveWidth() == static_cast<int32_t>(width) && out.effectiveHeight() == static_cast<int32_t>(height)) {
            return &out;
        }
    }

    return nullptr;
}

void App::updateGlobalBounds(const std::vector<WaylandOutputInfo>& outputs) {
    auto waylandOutputs = outputs.empty() ? waylandTracker.getOutputs() : outputs;

    std::lock_guard<std::mutex> lock(monitorsMutex);
    if (activeMonitors.empty()) return;

    if (!waylandOutputs.empty()) {
        for (auto& mon : activeMonitors) {
            if (mon->waylandOutputId == 0) continue;

            auto it = std::find_if(waylandOutputs.begin(), waylandOutputs.end(), [&](const WaylandOutputInfo& out) { return out.id == mon->waylandOutputId; });

            if (it != waylandOutputs.end()) {
                mon->config.pos_x = it->posX;
                mon->config.pos_y = it->posY;
                int32_t lw = it->effectiveWidth();
                int32_t lh = it->effectiveHeight();
                if (lw > 0 && lh > 0) {
                    mon->config.width = static_cast<uint32_t>(lw);
                    mon->config.height = static_cast<uint32_t>(lh);
                }
            }
        }
    }

    // Calculate overall desktop bounding box
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (!waylandOutputs.empty()) {
        minX = waylandOutputs[0].posX;
        minY = waylandOutputs[0].posY;
        int lw0 = waylandOutputs[0].effectiveWidth();
        int lh0 = waylandOutputs[0].effectiveHeight();
        maxX = waylandOutputs[0].posX + lw0;
        maxY = waylandOutputs[0].posY + lh0;

        for (const auto& out : waylandOutputs) {
            int lw = out.effectiveWidth();
            int lh = out.effectiveHeight();
            minX = std::min(minX, out.posX);
            minY = std::min(minY, out.posY);
            maxX = std::max(maxX, out.posX + lw);
            maxY = std::max(maxY, out.posY + lh);
        }
    }

    int extentW = maxX - minX;
    int extentH = maxY - minY;
    if (extentW <= 0 || extentH <= 0) {
        LOG_ERROR << "Extent of " << extentW << "x" << extentH << " is not valid!" << std::endl;
        return;
    }

    LOG << "Wayland native wl_output desktop layout: origin (" << minX << ", " << minY << "), extent: " << extentW << "x" << extentH << std::endl;

    for (auto& mon : activeMonitors) {
        if (mon->inputManager) {
            mon->inputManager->setDesktopGeometry(minX, minY, extentW, extentH, mon->config.pos_x, mon->config.pos_y, mon->config.width, mon->config.height);
        }
    }
}

void App::syncTrayMonitors() {
    if (!currentTrayManager) return;
    std::vector<SharedMonitorItem> items;
    {
        std::lock_guard<std::mutex> lock(monitorsMutex);
        for (const auto& mon : activeMonitors) {
            SharedMonitorItem item;
            item.id = mon->config.id;
            item.name = mon->config.name;
            item.nodeId = mon->config.node_id;
            item.isStreaming = mon->receivedPortalFrame || (mon->portalCapture && mon->portalCapture->isStreaming());
            item.isOverlayVisible = (mon->overlay != nullptr && mon->overlay->isOverlayVisible());
            item.enableDmaBuf = mon->config.enable_dmabuf;
            items.push_back(item);
        }
    }
    currentTrayManager->updateMonitors(items);
}

void App::startMonitorStream(MonitorConfig cfg) {
    auto mon = std::make_shared<CaptureInstance>(cfg);

    if (!mon->init()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(monitorsMutex);
        activeMonitors.push_back(mon);
    }

    updateGlobalBounds();
    syncTrayMonitors();

    mon->startCaptureThread(
        mon,
        [this](CaptureInstance& inst) {
            int32_t pwX = inst.portalCapture->getPositionX();
            int32_t pwY = inst.portalCapture->getPositionY();
            uint32_t pwW = inst.portalCapture->getStreamWidth();
            uint32_t pwH = inst.portalCapture->getStreamHeight();

            // Map PipeWire stream to Wayland output once
            auto waylandOutputs = waylandTracker.getOutputs();
            if (const auto* out = matchWaylandOutput(pwX, pwY, pwW, pwH, waylandOutputs)) {
                inst.waylandOutputId = out->id;
                if (!out->name.empty()) {
                    inst.config.name = out->name;
                    if (inst.overlay) {
                        std::lock_guard<std::mutex> rLock(inst.renderMutex);
                        inst.overlay->setOverlayName(out->name);
                    }
                }
            }

            updateGlobalBounds();
            configMgr.addMonitor(inst.config);
            syncTrayMonitors();
        },
        [this](CaptureInstance& inst) {
            {
                std::lock_guard<std::mutex> lock(monitorsMutex);
                activeMonitors.erase(std::remove_if(activeMonitors.begin(), activeMonitors.end(), [&](const std::shared_ptr<CaptureInstance>& m) { return m->config.id == inst.config.id; }),
                                     activeMonitors.end());
            }
            configMgr.removeMonitor(inst.config.id);
            updateGlobalBounds();
            syncTrayMonitors();
        });
}

void App::onAddMonitor() {
    LOG << "Add Monitor requested from tray..." << std::endl;
    MonitorConfig newCfg;
    newCfg.id = "mon_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    newCfg.name = "Unnamed Monitor";
    newCfg.enable_dmabuf = false;
    startMonitorStream(newCfg);
}

void App::onRemoveMonitor(const std::string& id) {
    LOG << "Remove Monitor requested for ID: " << id << std::endl;
    std::shared_ptr<CaptureInstance> target = nullptr;
    {
        std::lock_guard<std::mutex> lock(monitorsMutex);
        for (auto it = activeMonitors.begin(); it != activeMonitors.end(); ++it) {
            if ((*it)->config.id == id) {
                target = *it;
                activeMonitors.erase(it);
                break;
            }
        }
    }
    if (target) {
        target->destroy();
        configMgr.removeMonitor(id);
        updateGlobalBounds();
        syncTrayMonitors();
    }
}

int App::run() {
    LOG << "OpenVR Wayland Desktop starting..." << std::endl;

    if (!vr::VR_IsHmdPresent()) {
        LOG_ERROR << "No HMD found, exiting..." << std::endl;
        return -1;
    }

    vr::EVRInitError vrErr = vr::VRInitError_None;
    vr::VR_Init(&vrErr, vr::VRApplication_Overlay);
    if (vrErr != vr::VRInitError_None) {
        LOG_ERROR << "Failed to initialize OpenVR context: " << vr::VR_GetVRInitErrorAsEnglishDescription(vrErr) << std::endl;
        return -1;
    }

    if (configMgr.getInstallOpenVRApp().has_value()) {
        if (*configMgr.getInstallOpenVRApp()) {
            ManifestManager::installManifest(false);
        } else if (ManifestManager::isManifestInstalled()) {
            ManifestManager::uninstallManifest();
        }
    } else {
        if (ManifestManager::isManifestInstalled()) {
            ManifestManager::installManifest(false);
        }
    }

    if (vr::VRApplications()) {
        vr::VRApplications()->IdentifyApplication(0, ManifestManager::APP_KEY);
    }

    if (!waylandTracker.init()) {
        LOG_ERROR << "Warning: Could not initialize native Wayland display "
                     "tracker."
                  << std::endl;
    }

    if (!evdevTracker.init()) {
        LOG_ERROR << "Warning: Could not initialize Evdev physical mouse "
                     "tracker."
                  << std::endl;
    }

    waylandTracker.setOutputChangeCallback([this](const std::vector<WaylandOutputInfo>& outputs) { updateGlobalBounds(outputs); });

    evdevTracker.setPhysicalMouseCallback([this]() {
        std::lock_guard<std::mutex> lock(monitorsMutex);
        for (auto& mon : activeMonitors) {
            if (mon->overlay) {
                mon->overlay->suspendOverlayMouse();
            }
        }
    });

    TrayManager trayManager([this]() { onAddMonitor(); }, [this](const std::string& id) { onRemoveMonitor(id); });
    currentTrayManager = &trayManager;

    if (!trayManager.init()) {
        LOG_ERROR << "Failed to initialize Tray Manager on D-Bus." << std::endl;
        return -1;
    }

    trayManager.setManifestInstalled(ManifestManager::isManifestInstalled());
    trayManager.setManifestToggleCallback([this, &trayManager](bool install) {
        if (install) {
            bool success = ManifestManager::installManifest(true);
            LOG << "Tray toggled OpenVR app manifest: " << (success ? "installed" : "failed") << std::endl;
            configMgr.setInstallOpenVRApp(true);
        } else {
            bool success = ManifestManager::uninstallManifest();
            LOG << "Tray toggled OpenVR app manifest: " << (success ? "uninstalled" : "failed") << std::endl;
            configMgr.setInstallOpenVRApp(false);
        }
        trayManager.setManifestInstalled(ManifestManager::isManifestInstalled());
    });

    trayManager.setMonitorDmaBufToggleCallback([this](const std::string& id, bool enabled) {
        std::shared_ptr<CaptureInstance> target = nullptr;
        {
            std::lock_guard<std::mutex> lock(monitorsMutex);
            for (auto& mon : activeMonitors) {
                if (mon->config.id == id) {
                    target = mon;
                    break;
                }
            }
        }
        if (target) {
            LOG << "DMA-BUF toggled for monitor " << id << " (" << target->config.name << ") -> " << (enabled ? "enabled" : "disabled") << std::endl;
            target->setEnableDmaBuf(enabled);
            configMgr.addMonitor(target->config);
            configMgr.save();
            syncTrayMonitors();
        }
    });

    trayManager.setQuitCallback([]() {
        LOG << "Quit requested from system tray menu." << std::endl;
        g_appRunning = false;
    });

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const auto& savedMonitors = configMgr.getMonitors();
    if (!savedMonitors.empty()) {
        LOG << "Restoring " << savedMonitors.size() << " monitor(s) from config..." << std::endl;
        for (const auto& cfg : savedMonitors) {
            startMonitorStream(cfg);
        }
    } else {
        LOG << "No saved monitors found. Prompting for initial monitor selection..." << std::endl;
        onAddMonitor();
    }

    syncTrayMonitors();

    while (g_appRunning) {
        trayManager.processEvents();

        // Process VR overlay events
        {
            std::lock_guard<std::mutex> lock(monitorsMutex);
            for (auto& mon : activeMonitors) {
                if (mon->overlay) {
                    std::lock_guard<std::mutex> renderLock(mon->renderMutex);
                    mon->overlay->processEvents();
                }
            }
        }

        syncTrayMonitors();

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    LOG << "Shutting down..." << std::endl;
    currentTrayManager = nullptr;
    trayManager.stop();

    {
        std::lock_guard<std::mutex> lock(monitorsMutex);
        for (auto& mon : activeMonitors) {
            mon->destroy();
        }
        activeMonitors.clear();
    }

    vr::VR_Shutdown();
    VRVulkanOverlay::cleanupVulkan();
    return 0;
}

int main() {
    App app;
    return app.run();
}