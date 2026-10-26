
//
// vibecoded with gemini and claude
//

#include <iostream>
#include <string>
#include <array>
#include <algorithm>
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
using Clock = std::chrono::steady_clock;

// --- Configuration ---
const char*       OSC_HOST            = nullptr;  // nullptr = localhost
const char*       OSC_PORT            = "1234";
const std::string TARGET_NAME         = "Xbox Wireless Controller";
const std::string BASE_PATH           = "/";
const auto        HEARTBEAT_INTERVAL  = std::chrono::seconds(180);
const int         RUMBLE_DURATION_MS  = 200;
const int         RECONNECT_DELAY_S   = 3;
const auto        OSC_THROTTLE        = std::chrono::milliseconds(8);

// --- Clean shutdown via SIGINT / SIGTERM ---
static volatile sig_atomic_t g_running = 1;
static void on_signal(int) { g_running = 0; }

// Evdev events forwarded over OSC, with the address name each is sent to.
struct Route {
    __u16       type;
    __u16       code;
    const char* name;
};

const Route ROUTES[] = {
    {EV_ABS, ABS_X,      "ABS_X"},
    {EV_ABS, ABS_Y,      "ABS_Y"},
    {EV_ABS, ABS_Z,      "ABS_Z"},
    {EV_ABS, ABS_RZ,     "ABS_RZ"},
    {EV_ABS, ABS_BRAKE,  "ABS_BRAKE"},
    {EV_ABS, ABS_GAS,    "ABS_GAS"},
    {EV_ABS, ABS_HAT0X,  "ABS_HAT0X"},
    {EV_ABS, ABS_HAT0Y,  "ABS_HAT0Y"},
    {EV_KEY, BTN_TL,     "BTN_TL"},
    {EV_KEY, BTN_TR,     "BTN_TR"},
    {EV_KEY, BTN_SELECT, "BTN_SELECT"},
    {EV_KEY, BTN_START,  "BTN_START"},
    {EV_KEY, BTN_EAST,   "BTN_EAST"},
    {EV_KEY, BTN_SOUTH,  "BTN_SOUTH"},
    {EV_KEY, BTN_THUMBL, "BTN_THUMBL"},
    {EV_KEY, BTN_THUMBR, "BTN_THUMBR"},
    {EV_KEY, BTN_WEST,   "BTN_NORTH"}, // Physical N is reported as BTN_WEST
    {EV_KEY, BTN_NORTH,  "BTN_WEST"},  // Physical W is reported as BTN_NORTH
};

// Precomputed OSC addresses indexed directly by evdev code, so each event is
// a single array lookup. An empty string means the code isn't forwarded.
std::array<std::string, ABS_CNT> ABS_ADDR;
std::array<std::string, KEY_CNT> KEY_ADDR;

void build_address_tables() {
    for (const auto& r : ROUTES) {
        auto& addr = (r.type == EV_ABS) ? ABS_ADDR[r.code] : KEY_ADDR[r.code];
        addr = BASE_PATH + r.name;
    }
}

// Latest value per evdev code, waiting to be sent. Fixed-size storage plus a
// list of the codes that changed, so queueing and flushing never allocate and
// flushing only visits the codes that actually changed.
template <size_t N>
struct PendingValues {
    std::array<int, N>   value{};
    std::array<bool, N>  queued{};
    std::array<__u16, N> order{};
    size_t               count = 0;

    bool empty() const { return count == 0; }

    // Overwrites any stale value already queued for this code.
    void set(__u16 code, int v) {
        value[code] = v;
        if (!queued[code]) {
            queued[code]   = true;
            order[count++] = code;
        }
    }

    template <typename Fn>
    void flush(Fn&& send) {
        for (size_t i = 0; i < count; ++i) {
            const __u16 code = order[i];
            queued[code] = false;
            send(code, value[code]);
        }
        count = 0;
    }
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

    build_address_tables();

    lo_address target = lo_address_new(OSC_HOST, OSC_PORT);
    if (!target) {
        std::cerr << "Failed to create OSC address on port " << OSC_PORT << std::endl;
        return 1;
    }

    auto send = [target](const std::string& addr, int value) {
        if (lo_send(target, addr.c_str(), "i", value) < 0)
            std::cerr << "OSC send failed: " << lo_address_errstr(target) << std::endl;
    };
    auto send_axis   = [&](__u16 code, int value) { send(ABS_ADDR[code], value); };
    auto send_button = [&](__u16 code, int value) { send(KEY_ADDR[code], value); };

    auto last_heartbeat = Clock::now();
    auto last_flush     = Clock::now();

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
        Clock::time_point rumble_stop;

        // --- Event loop ---
        struct pollfd pfd = {};
        pfd.fd     = fd;
        pfd.events = POLLIN;

        // Axes (EV_ABS) are throttled: intermediate values are overwritten so only
        // the most recent position is sent per flush interval. This prevents flooding
        // the OSC receiver when multiple axes move simultaneously. Axes still queued
        // when the interval ends are flushed by the poll() timeout, so a final
        // resting position is never held back waiting for the next input event.
        //
        // Buttons (EV_KEY) are never throttled: every press and release is a discrete
        // edge event that must be delivered immediately on SYN_REPORT regardless of
        // whether the axis throttle interval has elapsed.
        PendingValues<ABS_CNT> pending_axes;
        PendingValues<KEY_CNT> pending_buttons;

        bool read_failed = false;

        while (g_running) {
            auto now = Clock::now();

            // Flush axes whose throttle interval has elapsed
            if (!pending_axes.empty() && now - last_flush >= OSC_THROTTLE) {
                pending_axes.flush(send_axis);
                last_flush = now;
            }

            // Stop rumble once its duration has elapsed
            if (rumbling && now >= rumble_stop) {
                remove_rumble(fd, rumble_id);
                rumble_id = -1;
                rumbling  = false;
            }

            // Fire heartbeat rumble on schedule
            if (!rumbling && now - last_heartbeat >= HEARTBEAT_INTERVAL) {
                rumble_id = start_rumble(fd);
                if (rumble_id >= 0) {
                    rumbling    = true;
                    rumble_stop = now + std::chrono::milliseconds(RUMBLE_DURATION_MS);
                }
                last_heartbeat = now;
            }

            // Sleep until input arrives or the next timed task is due, so an
            // idle controller costs no wakeups beyond the heartbeat.
            auto deadline = rumbling ? rumble_stop : last_heartbeat + HEARTBEAT_INTERVAL;
            if (!pending_axes.empty())
                deadline = std::min(deadline, last_flush + OSC_THROTTLE);
            const int timeout_ms = static_cast<int>(std::max<long long>(0,
                std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count()));

            const int ret = poll(&pfd, 1, timeout_ms);
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

            // Drain all queued events, many per read() call
            struct input_event evs[64];
            for (;;) {
                const ssize_t nread = read(fd, evs, sizeof(evs));
                if (nread < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "Read error: " << strerror(errno) << std::endl;
                        read_failed = true;
                    }
                    break;
                }

                const size_t n = static_cast<size_t>(nread) / sizeof(input_event);
                for (size_t i = 0; i < n; ++i) {
                    const input_event& ev = evs[i];
                    if (ev.type == EV_ABS) {
                        // Axis: accumulate, overwriting stale intermediate values.
                        if (ev.code < ABS_CNT && !ABS_ADDR[ev.code].empty())
                            pending_axes.set(ev.code, ev.value);
                    } else if (ev.type == EV_KEY) {
                        // Button: accumulate separately; never dropped by the throttle.
                        if (ev.code < KEY_CNT && !KEY_ADDR[ev.code].empty())
                            pending_buttons.set(ev.code, ev.value);
                    } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        // Always flush buttons immediately — every edge must be delivered.
                        pending_buttons.flush(send_button);

                        // Flush axes only once OSC_THROTTLE has elapsed.
                        const auto syn_now = Clock::now();
                        if (!pending_axes.empty() && syn_now - last_flush >= OSC_THROTTLE) {
                            pending_axes.flush(send_axis);
                            last_flush = syn_now;
                        }
                    }
                }

                // A short read means the kernel buffer is empty; skip the
                // extra read() that would only return EAGAIN.
                if (nread < static_cast<ssize_t>(sizeof(evs))) break;
            }
            if (read_failed) break;
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
