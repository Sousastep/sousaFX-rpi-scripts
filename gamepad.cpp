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
#include <atomic>
#include <memory>

namespace fs = std::filesystem;

// --- Configuration ---
const char* OSC_HOST             = nullptr;  // nullptr = localhost
const char* OSC_PORT             = "1234";
const std::string TARGET_NAME          = "Xbox Wireless Controller";
const std::string BASE_PATH            = "/";
const int         HEARTBEAT_INTERVAL_S = 180;
const int         RUMBLE_DURATION_MS   = 60;
const int         RECONNECT_DELAY_S    = 3;
const int         POLL_TIMEOUT_MS      = 50;

// --- Clean shutdown via atomic flags ---
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

// --- RAII Wrappers for safe resource management ---
struct FileDescriptor {
    int fd;
    FileDescriptor(int f = -1) : fd(f) {}
    ~FileDescriptor() { if (fd >= 0) close(fd); }
    
    operator int() const { return fd; }
    
    FileDescriptor& operator=(int f) {
        if (fd >= 0) close(fd);
        fd = f;
        return *this;
    }
    
    bool is_open() const { return fd >= 0; }
    
    void close_fd() {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
};

struct OscTarget {
    lo_address addr;
    OscTarget(const char* host, const char* port) { addr = lo_address_new(host, port); }
    ~OscTarget() { if (addr) lo_address_free(addr); }
    
    operator lo_address() const { return addr; }
    bool is_valid() const { return addr != nullptr; }
};

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

            FileDescriptor fd(open(path.c_str(), O_RDONLY | O_NONBLOCK));
            if (!fd.is_open()) continue;

            char name[256] = {};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);

            if (std::string(name).find(TARGET_NAME) != std::string::npos)
                return path;
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error scanning /dev/input: " << e.what() << "\n";
    }
    return "";
}

// Uploads a rumble effect and starts playing it. Returns the kernel effect
// ID (>= 0) on success, or -1 on failure.
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

// Set up robust signal handling using sigaction
void setup_signals() {
    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // Prevents interrupted system calls (like poll)
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

int main() {
    setup_signals();

    OscTarget target(OSC_HOST, OSC_PORT);
    if (!target.is_valid()) {
        std::cerr << "Failed to create OSC address on port " << OSC_PORT << "\n";
        return 1;
    }

    auto last_heartbeat = std::chrono::steady_clock::now();

    while (g_running) {
        // --- Device discovery ---
        const std::string path = find_controller();
        if (path.empty()) {
            std::cout << "Searching for \"" << TARGET_NAME << "\"...\n";
            sleep(RECONNECT_DELAY_S);
            continue;
        }

        FileDescriptor fd(open(path.c_str(), O_RDWR | O_NONBLOCK));
        if (!fd.is_open()) {
            std::cerr << "Failed to open " << path << ": " << strerror(errno) << "\n";
            sleep(RECONNECT_DELAY_S);
            continue;
        }
        std::cout << "Connected to " << path << "\n";

        // --- Rumble state ---
        int     rumble_id = -1;
        bool    rumbling  = false;
        std::chrono::steady_clock::time_point rumble_stop;

        // --- Event loop ---
        struct pollfd pfd = {};
        pfd.fd     = fd;
        pfd.events = POLLIN;

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

            const int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
            if (ret < 0) {
                if (errno == EINTR) continue;  // Re-check g_running on generic interrupts
                std::cerr << "poll() error: " << strerror(errno) << "\n";
                break;
            }
            if (ret == 0) continue;            // Timeout; loop back for housekeeping

            if (pfd.revents & (POLLERR | POLLHUP)) {
                std::cout << "Controller disconnected.\n";
                break;
            }

            // Drain all queued events and bundle them
            struct input_event ev;
            ssize_t nread;
            lo_bundle bundle = nullptr;

            while ((nread = read(fd, &ev, sizeof(ev))) > 0) {
                if (ev.type == EV_ABS || ev.type == EV_KEY) {
                    const auto it = EVENT_MAP.find(ev.code);
                    if (it != EVENT_MAP.end()) {
                        
                        // Initialize bundle lazily if we have valid events
                        if (!bundle) bundle = lo_bundle_new(LO_TT_IMMEDIATE);

                        lo_message msg = lo_message_new();
                        lo_message_add_int32(msg, ev.value);
                        lo_bundle_add_message(bundle, it->second.c_str(), msg);
                    }
                }
            }

            if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "Read error: " << strerror(errno) << "\n";
                if (bundle) lo_bundle_free(bundle);
                break;
            }

            // Send the batch of events in a single network packet
            if (bundle) {
                if (lo_send_bundle(target, bundle) < 0) {
                    std::cerr << "OSC bundle send failed.\n";
                }
                lo_bundle_free(bundle);
            }
        }

        // Clean up before attempting reconnect
        if (rumbling) remove_rumble(fd, rumble_id);
        fd.close_fd();

        if (g_running) sleep(RECONNECT_DELAY_S);
    }

    std::cout << "Shutting down.\n";
    return 0;
}