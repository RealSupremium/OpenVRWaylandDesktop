#pragma once

#include <linux/uinput.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

class InputManager {
   public:
    InputManager(int screenWidth, int screenHeight);
    ~InputManager();

    bool init();
    void setDesktopGeometry(int origX, int origY, int extW, int extH, int pX, int pY, int monW, int monH);
    void moveMouse(float x, float y);
    void sendClick(int button, bool pressed);
    void sendKey(uint16_t keycode, bool shift = false);
    void typeText(const std::string& text);
    void sendScroll(float xdelta, float ydelta);

    constexpr static const char* INPUT_NAME = "OpenVR Desktop Overlay Pointer";

   private:
    int uinput_fd = -1;
    int desktopOriginX = 0;
    int desktopOriginY = 0;
    int desktopExtentW = 0;
    int desktopExtentH = 0;
    int posX = 0;
    int posY = 0;
    int width = 0;
    int height = 0;
    float uinputScrollAccumX = 0.0f;
    float uinputScrollAccumY = 0.0f;
    float uinputHiResAccumX = 0.0f;
    float uinputHiResAccumY = 0.0f;
    uint64_t lastScrollTimeMs = 0;

    struct xkb_context* xkbContext = nullptr;
    struct xkb_keymap* xkbKeymap = nullptr;
    std::unordered_map<xkb_keysym_t, std::pair<uint16_t, bool>> keysymMap;

    void initXkb();
    void emit(uint16_t type, uint16_t code, int32_t val);
};
