#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "CaptureInstance.hpp"
#include "ConfigManager.hpp"
#include "EvdevTracker.hpp"
#include "TrayManager.hpp"
#include "WaylandDisplayTracker.hpp"

class App {
   public:
    App() = default;
    ~App() = default;

    int run();

   private:
    void startMonitorStream(MonitorConfig cfg);
    void updateGlobalBounds(const std::vector<WaylandOutputInfo>& outputs = {});
    void syncTrayMonitors();
    static const WaylandOutputInfo* matchWaylandOutput(int32_t posX, int32_t posY, uint32_t width, uint32_t height, const std::vector<WaylandOutputInfo>& waylandOutputs);

    void onAddMonitor();
    void onRemoveMonitor(const std::string& id);

    WaylandDisplayTracker waylandTracker;
    EvdevTracker evdevTracker;
    ConfigManager configMgr;
    TrayManager* currentTrayManager = nullptr;
    std::vector<std::shared_ptr<CaptureInstance>> activeMonitors;
    std::mutex monitorsMutex;
};
