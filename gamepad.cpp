
//
// vibecoded with gemini and claude
//

#include <iostream>
#include <string>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <linux/input.h>
#include <lo/lo.h>
#include <filesystem>
#include <chrono>
#include <cstring>

namespace fs = std::filesystem;

// --- Configuration ---
const char*       OSC_HOST            = nullptr;  // nullptr = localhost
const char*       OSC_PORT            = "1234";
const std::string TARGET_NAME         = "Xbox Wireless Controller";
const std::string BASE_PATH           = "/";
const int         HEARTBEAT_INTERVAL_S = 180;
const int         RUMBLE_DURATION_MS   = 200;
const int         RECONNECT_DELAY_S    = 3;
const int         POLL_TIMEOUT_MS      = 50;
const int         OSC_THROTTLE_MS      = 8;

// --- Clean shutdown via SIGINT / SIGTERM ---
static volatile sig_atomic_t g_running = 1;
static void on_signal(int) { g_running = 0; }

// Maps evdev codes to precomputed OSC address strings.
const std::unordered_map<int, std::string> EVENT_MAP = {
    {ABS_X,      BASE_PATH + "ABS_X"},
    {ABS_Y,      BASE_PATH + "ABS_Y"},
    {ABS_Z,      BASE_PATH + "ABS_Z"},
    {ABS_RZ,     BASE_PATH + "ABS_RZ"},
    {ABS_BRAKE,  BASE_PATH + "ABS_BRAKE"},
    {ABS_GAS,    BASE_PATH + "ABS_GAS"},
    {ABS_HAT0X,  BASE_PATH + "ABS_HAT0X"},
    {ABS_HAT0Y,  BASE_PATH + "ABS_HAT0Y"},
    {BTN_TL,     BASE_PATH + "BTN_TL"},
    {BTN_TR,     BASE_PATH + "BTN_TR"},
    {BTN_SELECT, BASE_PATH + "BTN_SELECT"},
    {BTN_START,  BASE_PATH + "BTN_START"},
    {BTN_EAST,   BASE_PATH + "BTN_EAST"},
    {BTN_SOUTH,  BASE_PATH + "BTN_SOUTH"},
    {BTN_THUMBL, BASE_PATH + "BTN_THUMBL"},
    {BTN_THUMBR, BASE_PATH + "BTN_THUMBR"},
    {BTN_WEST,   BASE_PATH + "BTN_NORTH"}, // Physical N is reported as BTN_WEST
    {BTN_NORTH,  BASE_PATH + "BTN_WEST"},  // Physical W is reported as BTN_NORTH
};

// Scans /dev/input for a device whose name contains TARGET_NAME.
std::string find_controller() {
    try {
        for (const auto& entry : fs::directory_iterator("/dev/input")) {
            const std::string path = entry.path().string();
            if (path.find("event") == std::string::npos) continue;

            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;

            char name[256] = {};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            close(fd);

            if (std::string(name).find(TARGET_NAME) != std::string::npos)
                return path;
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error scanning /dev/input: " << e.what() << std::endl;
    }
    return "";
}

// Uploads a rumble effect and starts playing it. Returns the kernel effect
// ID (>= 0) on success, or -1 on failure. The caller must call
// remove_rumble() after RUMBLE_DURATION_MS to release the effect slot.
int start_rumble(int fd) {
    struct ff_effect effect = {};
    effect.type                      = FF_RUMBLE;
    effect.id                        = -1;
    effect.replay.length             = static_cast<__u16>(RUMBLE_DURATION_MS);
    effect.replay.delay              = 0;
    effect.u.rumble.strong_magnitude = 0x0000;
    effect.u.rumble.weak_magnitude   = 0x2000;

    if (ioctl(fd, EVIOCSFF, &effect) == -1) return -1;

    struct input_event play = {};
    play.type  = EV_FF;
    play.code  = static_cast<__u16>(effect.id);
    play.value = 1;
    write(fd, &play, sizeof(play));

    return effect.id;
}

void remove_rumble(int fd, int effect_id) {
    if (effect_id >= 0) ioctl(fd, EVIOCRMFF, effect_id);
}

int main() {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    lo_address target = lo_address_new(OSC_HOST, OSC_PORT);
    if (!target) {
        std::cerr << "Failed to create OSC address on port " << OSC_PORT << std::endl;
        return 1;
    }

    auto last_heartbeat = std::chrono::steady_clock::now();
    auto last_flush     = std::chrono::steady_clock::now();

    while (g_running) {
        // --- Device discovery ---
        const std::string path = find_controller();
        if (path.empty()) {
            std::cout << "Searching for \"" << TARGET_NAME << "\"..." << std::endl;
            sleep(RECONNECT_DELAY_S);
            continue;
        }

        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            std::cerr << "Failed to open " << path << ": " << strerror(errno) << std::endl;
            sleep(RECONNECT_DELAY_S);
            continue;
        }
        std::cout << "Connected to " << path << std::endl;

        // --- Rumble state (managed non-blocking across loop iterations) ---
        int     rumble_id = -1;
        bool    rumbling  = false;
        std::chrono::steady_clock::time_point rumble_stop;

        // --- Event loop ---
        struct pollfd pfd = {};
        pfd.fd     = fd;
        pfd.events = POLLIN;

        // Axes (EV_ABS) are throttled: intermediate values are overwritten so only
        // the most recent position is sent per flush interval. This prevents flooding
        // the OSC receiver when multiple axes move simultaneously.
        //
        // Buttons (EV_KEY) are never throttled: every press and release is a discrete
        // edge event that must be delivered immediately on SYN_REPORT regardless of
        // whether the axis throttle interval has elapsed.
        std::unordered_map<int, int> pending_axes;
        std::unordered_map<int, int> pending_buttons;

        while (g_running) {
            auto now = std::chrono::steady_clock::now();

            // Stop rumble once its duration has elapsed
            if (rumbling && now >= rumble_stop) {
                remove_rumble(fd, rumble_id);
                rumble_id = -1;
                rumbling  = false;
            }

            // Fire heartbeat rumble on schedule
            if (!rumbling &&
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_heartbeat).count() >= HEARTBEAT_INTERVAL_S)
            {
                rumble_id = start_rumble(fd);
                if (rumble_id >= 0) {
                    rumbling    = true;
                    rumble_stop = now + std::chrono::milliseconds(RUMBLE_DURATION_MS);
                }
                last_heartbeat = now;
            }

            // Block until input arrives or timeout expires.
            // The short timeout keeps rumble/heartbeat bookkeeping responsive
            // without burning CPU when the controller is idle.
            const int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
            if (ret < 0) {
                if (errno == EINTR) continue;  // Signal interrupted; re-check g_running
                std::cerr << "poll() error: " << strerror(errno) << std::endl;
                break;
            }
            if (ret == 0) continue;            // Timeout; loop back for housekeeping

            if (pfd.revents & (POLLERR | POLLHUP)) {
                std::cout << "Controller disconnected." << std::endl;
                break;
            }

            // Drain all queued events in one go
            struct input_event ev;
            ssize_t nread;
            while ((nread = read(fd, &ev, sizeof(ev))) > 0) {
                if (ev.type == EV_ABS) {
                    // Axis: accumulate, overwriting stale intermediate values.
                    if (EVENT_MAP.count(ev.code))
                        pending_axes[ev.code] = ev.value;
                } else if (ev.type == EV_KEY) {
                    // Button: accumulate separately; never dropped by the throttle.
                    if (EVENT_MAP.count(ev.code))
                        pending_buttons[ev.code] = ev.value;
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    auto syn_now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        syn_now - last_flush).count();

                    // Always flush buttons immediately — every edge must be delivered.
                    for (const auto& [code, value] : pending_buttons) {
                        const std::string& addr = EVENT_MAP.at(code);
                        if (lo_send(target, addr.c_str(), "i", value) < 0)
                            std::cerr << "OSC send failed: " << lo_address_errstr(target) << std::endl;
                    }
                    pending_buttons.clear();

                    // Flush axes only once OSC_THROTTLE_MS has elapsed.
                    if (!pending_axes.empty() && elapsed >= OSC_THROTTLE_MS) {
                        for (const auto& [code, value] : pending_axes) {
                            const std::string& addr = EVENT_MAP.at(code);
                            if (lo_send(target, addr.c_str(), "i", value) < 0)
                                std::cerr << "OSC send failed: " << lo_address_errstr(target) << std::endl;
                        }
                        pending_axes.clear();
                        last_flush = syn_now;
                    }
                }
            }
            if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "Read error: " << strerror(errno) << std::endl;
                break;
            }
        }

        // Clean up before attempting reconnect
        if (rumbling) remove_rumble(fd, rumble_id);
        close(fd);

        if (g_running) sleep(RECONNECT_DELAY_S);
    }

    std::cout << "Shutting down." << std::endl;
    lo_address_free(target);
    return 0;
}