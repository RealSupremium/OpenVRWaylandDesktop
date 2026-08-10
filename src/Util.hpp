#pragma once

#include <string>
#include <string_view>

namespace Util {

constexpr std::string_view get_file_name(std::string_view path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
}

inline std::string getConfigDirectory() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg != '\0') {
        return std::string(xdg) + "/OpenVRWaylandDesktop";
    }
    const char* home = std::getenv("HOME");
    if (home && *home != '\0') {
        return std::string(home) + "/.config/OpenVRWaylandDesktop";
    }
    return ".config/OpenVRWaylandDesktop";
}

}  // namespace Util

#define LOG std::cout << "[" << Util::get_file_name(__FILE__) << "] "
#define LOG_ERROR std::cerr << "[" << Util::get_file_name(__FILE__) << "] "
