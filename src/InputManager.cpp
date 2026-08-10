#include "InputManager.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

#include "Util.hpp"

static uint64_t getNowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

InputManager::InputManager(int screenWidth, int screenHeight) : width(screenWidth), height(screenHeight) {}

InputManager::~InputManager() {
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
    }
    if (xkbKeymap) {
        xkb_keymap_unref(xkbKeymap);
        xkbKeymap = nullptr;
    }
    if (xkbContext) {
        xkb_context_unref(xkbContext);
        xkbContext = nullptr;
    }
}

void InputManager::initXkb() {
    xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkbContext) {
        LOG_ERROR << "Failed to create xkb_context" << std::endl;
        return;
    }

    xkbKeymap = xkb_keymap_new_from_names(xkbContext, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!xkbKeymap) {
        LOG_ERROR << "Failed to compile xkb_keymap" << std::endl;
        return;
    }

    for (xkb_keycode_t kc = xkb_keymap_min_keycode(xkbKeymap); kc <= xkb_keymap_max_keycode(xkbKeymap); ++kc) {
        if (kc < 8) continue;
        uint16_t evdevCode = static_cast<uint16_t>(kc - 8);

        const xkb_keysym_t* syms = nullptr;
        int numSyms = xkb_keymap_key_get_syms_by_level(xkbKeymap, kc, 0, 0, &syms);
        for (int s = 0; s < numSyms; ++s) {
            if (keysymMap.find(syms[s]) == keysymMap.end()) {
                keysymMap[syms[s]] = {evdevCode, false};
            }
        }

        numSyms = xkb_keymap_key_get_syms_by_level(xkbKeymap, kc, 0, 1, &syms);
        for (int s = 0; s < numSyms; ++s) {
            if (keysymMap.find(syms[s]) == keysymMap.end()) {
                keysymMap[syms[s]] = {evdevCode, true};
            }
        }
    }
}

bool InputManager::init() {
    initXkb();

    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        LOG_ERROR << "Failed to open /dev/uinput. Ensure user is in 'input' group or run with permissions." << std::endl;
        return false;
    }

    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_MIDDLE);

    for (int key = 1; key <= 240; ++key) {
        ioctl(uinput_fd, UI_SET_KEYBIT, key);
    }

    ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_X);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_Y);

    ioctl(uinput_fd, UI_SET_EVBIT, EV_REL);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_HWHEEL);
#ifdef REL_WHEEL_HI_RES
    ioctl(uinput_fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
#endif
#ifdef REL_HWHEEL_HI_RES
    ioctl(uinput_fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);
#endif

    ioctl(uinput_fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);

    struct uinput_setup usetup{};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    strcpy(usetup.name, INPUT_NAME);

    struct uinput_abs_setup abs_x{};
    abs_x.code = ABS_X;
    abs_x.absinfo.minimum = 0;
    abs_x.absinfo.maximum = 65535;
    ioctl(uinput_fd, UI_ABS_SETUP, &abs_x);

    struct uinput_abs_setup abs_y{};
    abs_y.code = ABS_Y;
    abs_y.absinfo.minimum = 0;
    abs_y.absinfo.maximum = 65535;
    ioctl(uinput_fd, UI_ABS_SETUP, &abs_y);

    ioctl(uinput_fd, UI_DEV_SETUP, &usetup);
    ioctl(uinput_fd, UI_DEV_CREATE);

    return true;
}

void InputManager::setDesktopGeometry(int origX, int origY, int extW, int extH, int pX, int pY, int monW, int monH) {
    desktopOriginX = origX;
    desktopOriginY = origY;
    desktopExtentW = extW;
    desktopExtentH = extH;
    posX = pX;
    posY = pY;
    width = monW;
    height = monH;
}

void InputManager::emit(uint16_t type, uint16_t code, int32_t val) {
    struct input_event ie{};
    ie.type = type;
    ie.code = code;
    ie.value = val;
    write(uinput_fd, &ie, sizeof(ie));
}

void InputManager::moveMouse(float u, float v) {
    // Suppress mouse motion events while scrolling (fixes scrolling in some apps like Steam)
    if (getNowMs() - lastScrollTimeMs < 100) return;

    float localX = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(width);
    float localY = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(height);

    float globalX = static_cast<float>(posX) + localX;
    float globalY = static_cast<float>(posY) + localY;

    float normX = std::clamp((globalX - static_cast<float>(desktopOriginX)) / static_cast<float>(desktopExtentW), 0.0f, 1.0f);
    float normY = std::clamp((globalY - static_cast<float>(desktopOriginY)) / static_cast<float>(desktopExtentH), 0.0f, 1.0f);

    int absX = static_cast<int>(normX * 65535.0f);
    int absY = static_cast<int>(normY * 65535.0f);

    if (uinput_fd >= 0) {
        emit(EV_ABS, ABS_X, absX);
        emit(EV_ABS, ABS_Y, absY);
        emit(EV_SYN, SYN_REPORT, 0);
    }
}

void InputManager::sendClick(int button, bool pressed) {
    if (uinput_fd < 0) return;
    uint16_t btn = (button == 1) ? BTN_RIGHT : ((button == 2) ? BTN_MIDDLE : BTN_LEFT);
    emit(EV_KEY, btn, pressed ? 1 : 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

void InputManager::sendKey(uint16_t keycode, bool shift) {
    if (uinput_fd < 0) return;

    if (shift) {
        emit(EV_KEY, KEY_LEFTSHIFT, 1);
        emit(EV_SYN, SYN_REPORT, 0);
    }

    emit(EV_KEY, keycode, 1);
    emit(EV_SYN, SYN_REPORT, 0);

    emit(EV_KEY, keycode, 0);
    emit(EV_SYN, SYN_REPORT, 0);

    if (shift) {
        emit(EV_KEY, KEY_LEFTSHIFT, 0);
        emit(EV_SYN, SYN_REPORT, 0);
    }
}

void InputManager::typeText(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        // Handle ANSI escape sequences sent by SteamVR for arrow keys & special keys
        if (text[i] == '\x1b') {
            if (i + 2 < text.size() && text[i + 1] == '[') {
                char code = text[i + 2];
                if (code == 'A') {
                    sendKey(KEY_UP);
                    i += 3;
                    continue;
                }
                if (code == 'B') {
                    sendKey(KEY_DOWN);
                    i += 3;
                    continue;
                }
                if (code == 'C') {
                    sendKey(KEY_RIGHT);
                    i += 3;
                    continue;
                }
                if (code == 'D') {
                    sendKey(KEY_LEFT);
                    i += 3;
                    continue;
                }
                if (code == 'H') {
                    sendKey(KEY_HOME);
                    i += 3;
                    continue;
                }
                if (code == 'F') {
                    sendKey(KEY_END);
                    i += 3;
                    continue;
                }

                if (i + 3 < text.size() && text[i + 3] == '~') {
                    if (code == '1') {
                        sendKey(KEY_HOME);
                        i += 4;
                        continue;
                    }
                    if (code == '3') {
                        sendKey(KEY_DELETE);
                        i += 4;
                        continue;
                    }
                    if (code == '4') {
                        sendKey(KEY_END);
                        i += 4;
                        continue;
                    }
                    if (code == '5') {
                        sendKey(KEY_PAGEUP);
                        i += 4;
                        continue;
                    }
                    if (code == '6') {
                        sendKey(KEY_PAGEDOWN);
                        i += 4;
                        continue;
                    }
                }
            }
            sendKey(KEY_ESC);
            i++;
            continue;
        }

        char c = text[i++];
        if (c == '\b' || c == '\x7f') {
            sendKey(KEY_BACKSPACE);
            continue;
        }
        if (c == '\r' || c == '\n') {
            sendKey(KEY_ENTER);
            continue;
        }
        if (c == '\t') {
            sendKey(KEY_TAB);
            continue;
        }

        xkb_keysym_t sym = xkb_utf32_to_keysym(static_cast<uint32_t>(static_cast<unsigned char>(c)));
        if (sym == XKB_KEY_NoSymbol) {
            continue;
        }

        auto it = keysymMap.find(sym);
        if (it != keysymMap.end()) {
            sendKey(it->second.first, it->second.second);
        }
    }
}

void InputManager::sendScroll(float xdelta, float ydelta) {
    lastScrollTimeMs = getNowMs();

    if (uinput_fd < 0) return;

    bool emitted = false;

    if (std::abs(ydelta) > 5.0f) ydelta /= 120.0f;
    if (std::abs(xdelta) > 5.0f) xdelta /= 120.0f;

    if (std::abs(ydelta) > 0.0001f) {
        if ((ydelta > 0.0f && uinputHiResAccumY < 0.0f) || (ydelta < 0.0f && uinputHiResAccumY > 0.0f)) {
            uinputHiResAccumY = 0.0f;
            uinputScrollAccumY = 0.0f;
        }

        uinputHiResAccumY += ydelta * 120.0f;
        uinputScrollAccumY += ydelta;

        const int hiResSteps = static_cast<int>(std::trunc(uinputHiResAccumY));
        if (hiResSteps != 0) {
            uinputHiResAccumY -= static_cast<float>(hiResSteps);
            if (std::abs(uinputHiResAccumY) < 0.001f) uinputHiResAccumY = 0.0f;

#ifdef REL_WHEEL_HI_RES
            emit(EV_REL, REL_WHEEL_HI_RES, hiResSteps);
            emitted = true;
#endif
        }

        if (std::abs(uinputScrollAccumY) >= 0.25f) {
            const int wheelSteps = uinputScrollAccumY > 0.0f ? 1 : -1;
            uinputScrollAccumY -= static_cast<float>(wheelSteps) * 0.25f;
            if (std::abs(uinputScrollAccumY) < 0.001f) uinputScrollAccumY = 0.0f;

            emit(EV_REL, REL_WHEEL, wheelSteps);
            emitted = true;
        }
    }

    if (std::abs(xdelta) > 0.0001f) {
        if ((xdelta > 0.0f && uinputHiResAccumX < 0.0f) || (xdelta < 0.0f && uinputHiResAccumX > 0.0f)) {
            uinputHiResAccumX = 0.0f;
            uinputScrollAccumX = 0.0f;
        }

        uinputHiResAccumX += xdelta * 120.0f;
        uinputScrollAccumX += xdelta;

        const int hiResSteps = static_cast<int>(std::trunc(uinputHiResAccumX));
        if (hiResSteps != 0) {
            uinputHiResAccumX -= static_cast<float>(hiResSteps);
            if (std::abs(uinputHiResAccumX) < 0.001f) uinputHiResAccumX = 0.0f;

#ifdef REL_HWHEEL_HI_RES
            emit(EV_REL, REL_HWHEEL_HI_RES, hiResSteps);
            emitted = true;
#endif
        }

        if (std::abs(uinputScrollAccumX) >= 0.25f) {
            const int wheelSteps = uinputScrollAccumX > 0.0f ? 1 : -1;
            uinputScrollAccumX -= static_cast<float>(wheelSteps) * 0.25f;
            if (std::abs(uinputScrollAccumX) < 0.001f) uinputScrollAccumX = 0.0f;

            emit(EV_REL, REL_HWHEEL, wheelSteps);
            emitted = true;
        }
    }

    if (emitted) emit(EV_SYN, SYN_REPORT, 0);
}
