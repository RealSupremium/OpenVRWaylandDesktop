#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

struct MonitorConfig {
    std::string id;
    std::string name;
    std::string restore_token;
    uint32_t node_id = 0;
    int32_t pos_x = 0;
    int32_t pos_y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool enable_dmabuf = false;
};

// @clang-format off
inline void to_json(nlohmann::json& j, const MonitorConfig& m) {
    j = nlohmann::json{{"id", m.id},
                       {"name", m.name},
                       {"restore_token", m.restore_token},
                       {"node_id", m.node_id},
                       {"pos_x", m.pos_x},
                       {"pos_y", m.pos_y},
                       {"width", m.width},
                       {"height", m.height},
                       {"enable_dmabuf", m.enable_dmabuf}};
}
// @clang-format on

inline void from_json(const nlohmann::json& j, MonitorConfig& m) {
    m.id = j.value("id", "");
    m.name = j.value("name", "");
    m.restore_token = j.value("restore_token", "");
    m.node_id = j.value("node_id", 0u);
    m.pos_x = j.value("pos_x", 0);
    m.pos_y = j.value("pos_y", 0);
    m.width = j.value("width", 0u);
    m.height = j.value("height", 0u);
    m.enable_dmabuf = j.value("enable_dmabuf", false);
}

class ConfigManager {
   public:
    ConfigManager();
    ~ConfigManager() = default;

    bool load();
    bool save();

    void addMonitor(const MonitorConfig& mon);
    void removeMonitor(const std::string& id);
    const std::vector<MonitorConfig>& getMonitors() const { return monitors; }

    void setInstallOpenVRApp(bool install);
    std::optional<bool> getInstallOpenVRApp() const { return installOpenVRApp; }

    std::filesystem::path getConfigFilePath() const;

   private:
    std::vector<MonitorConfig> monitors;
    std::optional<bool> installOpenVRApp;
};
