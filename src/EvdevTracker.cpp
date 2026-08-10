#include "EvdevTracker.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#include "InputManager.hpp"
#include "Util.hpp"

#ifndef BITS_PER_LONG
#define BITS_PER_LONG (sizeof(long) * 8)
#endif
#ifndef NBITS
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
#endif
#ifndef OFF
#define OFF(x) ((x) % BITS_PER_LONG)
#endif
#ifndef LONG
#define LONG(x) ((x) / BITS_PER_LONG)
#endif
#ifndef TEST_BIT
#define TEST_BIT(bit, array) (((array)[LONG(bit)] >> OFF(bit)) & 1U)
#endif

EvdevTracker::EvdevTracker() {}

EvdevTracker::~EvdevTracker() { stop(); }

void EvdevTracker::notifyPhysicalMouseMotion() {
    PhysicalMouseCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        cbCopy = physicalMouseCallback;
    }
    if (cbCopy) {
        cbCopy();
    }
}

void EvdevTracker::scanInputDevices() {
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        LOG_ERROR << "ERROR: Cannot open /dev/input directory: " << strerror(errno) << std::endl;
        return;
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        std::string path = std::string("/dev/input/") + ent->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            LOG_ERROR << "Warning: Cannot open " << path << ": " << strerror(errno) << std::endl;
            continue;
        }

        char name[256] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);

        struct input_id devId{};
        ioctl(fd, EVIOCGID, &devId);

        unsigned long evbit[NBITS(EV_MAX)] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
            LOG_ERROR << "Skip " << path << " (" << name << "): EVIOCGBIT failed" << std::endl;
            close(fd);
            continue;
        }

        bool hasRel = TEST_BIT(EV_REL, evbit);
        bool hasKey = TEST_BIT(EV_KEY, evbit);

        if (!hasRel && !hasKey) {
            close(fd);
            continue;
        }

        bool isPointerDevice = false;
        if (hasRel) {
            unsigned long relbit[NBITS(REL_MAX)] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit) >= 0) {
                if (TEST_BIT(REL_X, relbit) || TEST_BIT(REL_Y, relbit)) {
                    isPointerDevice = true;
                }
            }
        }

        if (!isPointerDevice && hasKey) {
            unsigned long keybit[NBITS(KEY_MAX)] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
                if (TEST_BIT(BTN_LEFT, keybit) || TEST_BIT(BTN_MOUSE, keybit)) {
                    isPointerDevice = true;
                }
            }
        }

        if (!isPointerDevice) {
            close(fd);
            continue;
        }

        if (devId.bustype == BUS_VIRTUAL || (devId.vendor == 0x1234 && devId.product == 0x5678) || strstr(name, InputManager::INPUT_NAME) != nullptr) {
            LOG << "Ignored virtual device: " << path << " (" << name << ")" << std::endl;
            close(fd);
            continue;
        }

        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            openFds.push_back(fd);
            LOG << "Tracking physical pointer device: " << path << " (" << name << ")" << std::endl;
        } else {
            LOG_ERROR << "Failed to add " << path << " to epoll: " << strerror(errno) << std::endl;
            close(fd);
        }
    }
    closedir(dir);
}

bool EvdevTracker::init() {
    epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd < 0) {
        LOG_ERROR << "Failed to create evdev epoll fd." << std::endl;
        return false;
    }

    stopFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stopFd < 0) {
        LOG_ERROR << "Failed to create evdev stop eventfd." << std::endl;
        close(epollFd);
        epollFd = -1;
        return false;
    }

    struct epoll_event stopEv{};
    stopEv.events = EPOLLIN;
    stopEv.data.fd = stopFd;
    epoll_ctl(epollFd, EPOLL_CTL_ADD, stopFd, &stopEv);

    scanInputDevices();

    LOG << "Evdev physical mouse tracker monitoring " << openFds.size() << " hardware input device(s)." << std::endl;

    running = true;
    thread = std::thread([this]() {
        struct epoll_event events[16];
        while (running) {
            int nfds = epoll_wait(epollFd, events, 16, 500);
            if (nfds < 0) {
                if (errno == EINTR) continue;
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                if (fd == stopFd) {
                    running = false;
                    break;
                }

                struct input_event evs[32];
                ssize_t bytes = read(fd, evs, sizeof(evs));
                if (bytes <= 0) continue;

                size_t count = bytes / sizeof(struct input_event);
                bool physicalMotionDetected = false;

                for (size_t k = 0; k < count; ++k) {
                    const auto& ie = evs[k];
                    if (ie.type == EV_REL) {
                        if ((ie.code == REL_X || ie.code == REL_Y) && ie.value != 0) {
                            physicalMotionDetected = true;
                            break;
                        }
                    } else if (ie.type == EV_KEY) {
                        if (ie.code == BTN_LEFT || ie.code == BTN_RIGHT || ie.code == BTN_MIDDLE || ie.code == BTN_SIDE || ie.code == BTN_EXTRA) {
                            physicalMotionDetected = true;
                            break;
                        }
                    }
                }

                if (physicalMotionDetected) {
                    notifyPhysicalMouseMotion();
                }
            }
        }
    });

    return true;
}

void EvdevTracker::stop() {
    bool expected = true;
    if (!running.compare_exchange_strong(expected, false)) {
        return;
    }

    if (stopFd >= 0) {
        uint64_t v = 1;
        write(stopFd, &v, sizeof(v));
    }

    if (thread.joinable()) {
        thread.join();
    }

    if (stopFd >= 0) {
        close(stopFd);
        stopFd = -1;
    }

    if (epollFd >= 0) {
        close(epollFd);
        epollFd = -1;
    }

    for (int fd : openFds) {
        if (fd >= 0) close(fd);
    }
    openFds.clear();
}
