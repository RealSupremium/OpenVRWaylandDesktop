#include "ConfigManager.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

#include "Util.hpp"

ConfigManager::ConfigManager() { load(); }

std::filesystem::path ConfigManager::getConfigFilePath() const { return std::filesystem::path(Util::getConfigDirectory()) / "config.json"; }

bool ConfigManager::save() {
    try {
        auto filePath = getConfigFilePath();
        std::filesystem::create_directories(filePath.parent_path());

        std::ofstream out(filePath);
        if (!out.is_open()) {
            LOG_ERROR << "Failed to open config file for writing: " << filePath << std::endl;
            return false;
        }

        nlohmann::json j;
        j["monitors"] = monitors;
        if (installOpenVRApp.has_value()) {
            j["install_openvr_app"] = *installOpenVRApp;
        }

        out << j.dump(2) << std::endl;

        LOG << "Configuration saved to " << filePath << std::endl;
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "Exception saving config: " << e.what() << std::endl;
        return false;
    }
}

bool ConfigManager::load() {
    monitors.clear();
    auto filePath = getConfigFilePath();
    if (!std::filesystem::exists(filePath)) {
        return false;
    }

    try {
        std::ifstream in(filePath);
        if (!in.is_open()) return false;

        nlohmann::json j = nlohmann::json::parse(in);
        if (j.contains("install_openvr_app") && j["install_openvr_app"].is_boolean()) {
            installOpenVRApp = j["install_openvr_app"].get<bool>();
        }

        if (j.contains("monitors") && j["monitors"].is_array()) {
            for (const auto& item : j["monitors"]) {
                MonitorConfig mon = item.get<MonitorConfig>();
                if (!mon.restore_token.empty()) {
                    if (mon.id.empty()) mon.id = "mon_" + std::to_string(monitors.size() + 1);
                    if (mon.name.empty()) mon.name = "Monitor " + std::to_string(monitors.size() + 1);
                    monitors.push_back(mon);
                }
            }
        }

        LOG << "Loaded " << monitors.size() << " saved monitor configuration(s) from " << filePath << std::endl;
        return !monitors.empty();
    } catch (const std::exception& e) {
        LOG_ERROR << "Exception loading config: " << e.what() << std::endl;
        return false;
    }
}

void ConfigManager::addMonitor(const MonitorConfig& mon) {
    auto it = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorConfig& m) { return m.id == mon.id || (!mon.restore_token.empty() && m.restore_token == mon.restore_token); });
    if (it != monitors.end()) {
        *it = mon;
    } else {
        monitors.push_back(mon);
    }
    save();
}

void ConfigManager::removeMonitor(const std::string& id) {
    monitors.erase(std::remove_if(monitors.begin(), monitors.end(), [&](const MonitorConfig& m) { return m.id == id || m.restore_token == id; }), monitors.end());
    save();
}

void ConfigManager::setInstallOpenVRApp(bool install) {
    installOpenVRApp = install;
    save();
}
