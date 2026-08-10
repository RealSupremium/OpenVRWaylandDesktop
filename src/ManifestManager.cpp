#include "ManifestManager.hpp"

#include <linux/limits.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "Util.hpp"

std::filesystem::path ManifestManager::getExecutablePath() {
    const char* appImage = std::getenv("APPIMAGE");
    if (appImage && *appImage != '\0') {
        return std::filesystem::path(appImage);
    }

    try {
        std::filesystem::path exePath = std::filesystem::read_symlink("/proc/self/exe");
        if (!exePath.empty()) {
            return std::filesystem::canonical(exePath);
        }
    } catch (...) {
    }

    char buffer[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (count != -1) {
        buffer[count] = '\0';
        return std::filesystem::path(buffer);
    }

    return std::filesystem::current_path() / "OpenVRWaylandDesktop";
}

std::filesystem::path ManifestManager::getManifestPath() { return std::filesystem::path(Util::getConfigDirectory()) / "OpenVRWaylandDesktop.vrmanifest"; }

std::filesystem::path ManifestManager::getIconPath() {
    std::filesystem::path configDir = getManifestPath().parent_path();
    std::filesystem::path targetIcon = configDir / "openvr_desktop_overlay.png";

    std::error_code ec;
    if (std::filesystem::exists(targetIcon, ec) && std::filesystem::file_size(targetIcon, ec) > 0) {
        return std::filesystem::canonical(targetIcon, ec);
    }

    std::vector<std::filesystem::path> candidates;

    const char* appDir = std::getenv("APPDIR");
    if (appDir && *appDir != '\0') {
        std::filesystem::path appDirPath(appDir);
        candidates.push_back(appDirPath / "usr" / "share" / "icons" / "hicolor" / "512x512" / "apps" / "openvr_desktop_overlay.png");
    }

    std::filesystem::path exePath = getExecutablePath();
    candidates.push_back(exePath.parent_path() / "resources" / "openvr_desktop_overlay.png");
    candidates.push_back(exePath.parent_path() / ".." / "resources" / "openvr_desktop_overlay.png");
    candidates.push_back(exePath.parent_path() / "openvr_desktop_overlay.png");

    for (const auto& candidate : candidates) {
        std::error_code candidateEc;
        if (std::filesystem::exists(candidate, candidateEc) && std::filesystem::file_size(candidate, candidateEc) > 0) {
            try {
                std::filesystem::create_directories(configDir);
                std::filesystem::copy_file(candidate, targetIcon, std::filesystem::copy_options::overwrite_existing);
                LOG << "Found icon at " << candidate << " and cached to " << targetIcon << std::endl;
                return std::filesystem::canonical(targetIcon);
            } catch (const std::exception& e) {
                LOG_ERROR << "Failed to copy icon from " << candidate << " to " << targetIcon << ": " << e.what() << std::endl;
                return std::filesystem::canonical(candidate, candidateEc);
            }
        }
    }

    LOG_ERROR << "Could not find openvr_desktop_overlay.png in any candidate path!" << std::endl;
    return targetIcon;
}

bool ManifestManager::isManifestInstalled() {
    if (!vr::VRApplications()) return false;
    return vr::VRApplications()->IsApplicationInstalled(APP_KEY);
}

bool ManifestManager::setAutoLaunch(bool enable) {
    if (!vr::VRApplications()) return false;
    vr::EVRApplicationError err = vr::VRApplications()->SetApplicationAutoLaunch(APP_KEY, enable);
    if (err != vr::VRApplicationError_None) {
        LOG_ERROR << "SetApplicationAutoLaunch error: " << vr::VRApplications()->GetApplicationsErrorNameFromEnum(err) << std::endl;
        return false;
    }
    return true;
}

bool ManifestManager::installManifest(bool forceReinstall) {
    if (!vr::VRApplications()) {
        LOG_ERROR << "Cannot install manifest: OpenVR VRApplications interface unavailable." << std::endl;
        return false;
    }

    std::filesystem::path exePath = getExecutablePath();
    std::filesystem::path manifestPath = getManifestPath();
    std::filesystem::path iconPath = getIconPath();

    if (!forceReinstall && isManifestInstalled() && std::filesystem::exists(manifestPath)) {
        std::ifstream in(manifestPath);
        if (in.is_open()) {
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (content.find(exePath.string()) != std::string::npos && content.find("image_path") != std::string::npos) {
                LOG << "SteamVR application manifest is up to date: " << exePath << std::endl;
                setAutoLaunch(true);
                return true;
            }
        }
    }

    try {
        std::filesystem::create_directories(manifestPath.parent_path());
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to create directories for manifest: " << e.what() << std::endl;
        return false;
    }

    std::ofstream out(manifestPath);
    if (!out.is_open()) {
        LOG_ERROR << "Failed to open manifest file for writing: " << manifestPath << std::endl;
        return false;
    }

    nlohmann::json manifest = {
        {"source", "builtin"},
        {"applications", nlohmann::json::array({{{"app_key", APP_KEY},
                                                 {"launch_type", "binary"},
                                                 {"binary_path_linux", exePath.string()},
                                                 {"is_dashboard_overlay", true},
                                                 {"image_path", iconPath.string()},
                                                 {"strings", {{"en_us", {{"name", "OpenVR Wayland Desktop"}, {"description", "A Wayland desktop overlay for SteamVR on Linux"}}}}}}})}};

    out << manifest.dump(2) << std::endl;
    out.close();

    LOG << "Wrote application manifest to: " << manifestPath << std::endl;

    std::string manifestPathStr = manifestPath.string();
    vr::EVRApplicationError err = vr::VRApplications()->AddApplicationManifest(manifestPathStr.c_str(), false);
    if (err != vr::VRApplicationError_None) {
        LOG_ERROR << "Failed to register application manifest with OpenVR: " << vr::VRApplications()->GetApplicationsErrorNameFromEnum(err) << std::endl;
        return false;
    }

    if (!setAutoLaunch(true)) {
        LOG_ERROR << "Warning: Manifest added but failed to enable auto-launch." << std::endl;
    } else {
        LOG << "Successfully registered manifest & enabled auto-launch for SteamVR!" << std::endl;
    }

    return true;
}

bool ManifestManager::uninstallManifest() {
    if (!vr::VRApplications()) {
        LOG_ERROR << "Cannot uninstall manifest: OpenVR VRApplications interface unavailable." << std::endl;
        return false;
    }

    std::filesystem::path manifestPath = getManifestPath();
    std::string manifestPathStr = manifestPath.string();

    if (isManifestInstalled()) {
        vr::EVRApplicationError err = vr::VRApplications()->RemoveApplicationManifest(manifestPathStr.c_str());
        if (err != vr::VRApplicationError_None) {
            LOG_ERROR << "Failed to remove application manifest: " << vr::VRApplications()->GetApplicationsErrorNameFromEnum(err) << std::endl;
        } else {
            LOG << "Removed application manifest from SteamVR." << std::endl;
        }
    }

    if (std::filesystem::exists(manifestPath)) {
        std::error_code ec;
        std::filesystem::remove(manifestPath, ec);
        if (!ec) {
            LOG << "Deleted manifest file: " << manifestPath << std::endl;
        }
    }

    return true;
}
