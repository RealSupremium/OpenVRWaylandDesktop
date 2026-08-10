#pragma once

#include <openvr.h>

#include <filesystem>

class ManifestManager {
   public:
    static constexpr const char* APP_KEY = "openvr_wayland_desktop";

    static std::filesystem::path getManifestPath();
    static std::filesystem::path getExecutablePath();
    static std::filesystem::path getIconPath();
    static bool installManifest(bool forceReinstall = false);
    static bool uninstallManifest();
    static bool isManifestInstalled();
    static bool setAutoLaunch(bool enable);
};
