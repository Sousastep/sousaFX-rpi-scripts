
//
//   https://gemini.google.com/share/22e1ec2577e7
//

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <lo/lo.h>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

// --- Configuration ---
const std::string TARGET_NAME = "Xbox Wireless Controller";
const char* OSC_PORT = "1234";
const std::string BASE_PATH = "/rnbo/inst/0/params/gpin/";
const int HEARTBEAT_INTERVAL = 60; // Seconds

// Maps evdev codes to OSC suffixes
std::map<int, std::string> EVENT_MAP = {
    {ABS_X, "ABS_X"}, 
    {ABS_Y, "ABS_Y"}, 
    {ABS_Z, "ABS_Z"}, 
    {ABS_RZ, "ABS_RZ"},
    {ABS_BRAKE, "ABS_BRAKE"}, 
    {ABS_GAS, "ABS_GAS"}, 
    {ABS_HAT0X, "ABS_HAT0X"}, 
    {ABS_HAT0Y, "ABS_HAT0Y"},
    {BTN_TL, "BTN_TL"}, 
    {BTN_TR, "BTN_TR"}, 
    {BTN_SELECT, "BTN_SELECT"}, 
    {BTN_START, "BTN_START"},
    {BTN_EAST, "BTN_EAST"}, 
    {BTN_SOUTH, "BTN_SOUTH"}, 
    {BTN_THUMBL, "BTN_THUMBL"}, 
    {BTN_THUMBR, "BTN_THUMBR"},
    {BTN_WEST, "BTN_NORTH"}, // N & W swapped for some reason
    {BTN_NORTH, "BTN_WEST"} //
};

std::string find_controller() {
    for (const auto& entry : fs::directory_iterator("/dev/input")) {
        std::string path = entry.path().string();
        if (path.find("event") != std::string::npos) {
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                char name[256] = "Unknown";
                ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                close(fd);
                if (std::string(name).find(TARGET_NAME) != std::string::npos) return path;
            }
        }
    }
    return "";
}

void send_rumble(int fd) {
    struct ff_effect effect;
    effect.type = FF_RUMBLE;
    effect.id = -1;
    effect.replay.length = 200;
    effect.replay.delay = 0;
    effect.u.rumble.strong_magnitude = 0x0000;
    effect.u.rumble.weak_magnitude = 0x2000;

    if (ioctl(fd, EVIOCSFF, &effect) != -1) {
        struct input_event play;
        play.type = EV_FF;
        play.code = effect.id;
        play.value = 1;
        write(fd, &play, sizeof(play));
        
        usleep(200000); // 0.2s
        ioctl(fd, EVIOCRMFF, effect.id);
    }
}

int main() {
    lo_address target = lo_address_new(NULL, OSC_PORT);
    auto last_heartbeat = std::chrono::steady_clock::now();

    while (true) {
        std::string path = find_controller();
        if (path.empty()) {
            std::cout << "Searching for " << TARGET_NAME << "..." << std::endl;
            sleep(3);
            continue;
        }

        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) { sleep(3); continue; }
        std::cout << "Connected to " << path << std::endl;

        struct input_event ev;
        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count() > HEARTBEAT_INTERVAL) {
                send_rumble(fd);
                last_heartbeat = now;
            }

            if (read(fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_ABS || ev.type == EV_KEY) {
                    if (EVENT_MAP.count(ev.code)) {
                        std::string address = BASE_PATH + EVENT_MAP[ev.code];
                        lo_send(target, address.c_str(), "i", ev.value);
                    }
                }
            } else if (errno != EAGAIN) {
                std::cout << "Controller disconnected." << std::endl;
                break;
            }
            usleep(1000); // Prevent 100% CPU usage
        }
        close(fd);
    }
    lo_address_free(target);
    return 0;
}