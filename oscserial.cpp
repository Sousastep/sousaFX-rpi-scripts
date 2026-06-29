#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sched.h>      // sched_setscheduler, SCHED_FIFO
#include <pthread.h>    // pthread_setaffinity_np
#include <lo/lo.h>

// --- Configuration ---
const char*    SERIAL_PORT  = "/dev/ttyACM0";
const speed_t  BAUD_RATE    = B115200;
const int      FPS          = 260;

// How many consecutive write failures to suppress before logging one.
// At 260 FPS a dropped connection would otherwise flood stderr.
static constexpr int WRITE_ERR_REPORT_INTERVAL = 260;

struct ParamConfig {
    int         index;
    std::string route;
};

const std::vector<ParamConfig> PARAMS = {
    {0,  "/brightness"}, {1,  "/radius"},    {2,  "/palette"},
    {3,  "/divisionsHi"},{4,  "/divisionsLo"},{5,  "/width"},
    {6,  "/curve"},      {7,  "/rotation"},   {8,  "/fadeIn"},
    {9,  "/fadeOut"},    {10, "/peakPosition"},{11, "/pattern"},
    {12, "/gradientOffset"},{13, "/maskType"}
};

// --- Global State ---
// Each element explicitly value-initialised to 0 for portability across
// C++17 and C++20 (aggregate initialisation of atomics was underspecified
// in C++17).
std::array<std::atomic<uint8_t>, 14> param_data;

// --- OSC Callback Handler ---
int generic_handler(const char* /*path*/, const char* /*types*/,
                    lo_arg** argv, int /*argc*/,
                    void* /*data*/, void* user_data)
{
    int index = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    if (index >= 0 && index < 14) {
        param_data[index].store(static_cast<uint8_t>(argv[0]->i),
                                std::memory_order_relaxed);
    }
    return 0;
}

void error(int num, const char* msg, const char* path) {
    std::cerr << "Liblo Server Error " << num
              << " in " << (path ? path : "unknown")
              << ": " << msg << "\n";
}

// --- Serial Setup ---
// O_NONBLOCK prevents write() from blocking if the serial buffer is full,
// keeping the timing loop from being stalled by a slow or stalled device.
int setup_serial(const char* port) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }

    cfsetospeed(&tty, BAUD_RATE);
    cfsetispeed(&tty, BAUD_RATE);

    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag  = 0;
    tty.c_oflag  = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5;
    tty.c_cflag |=  (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { close(fd); return -1; }
    return fd;
}

// --- Real-time thread setup ---
// Pins the calling thread to core 3 (the last Pi core, usually least
// contested by the OS) and raises it to SCHED_FIFO priority 50.
// Falls back gracefully if privileges are insufficient (no sudo).
static void configure_realtime_thread() {
#ifdef __linux__
    // Pin to CPU core 3
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        std::cerr << "[warn] Could not set CPU affinity (non-fatal)\n";
    }

    // Raise scheduling priority
    struct sched_param sp{};
    sp.sched_priority = 50; // range: 1–99 for SCHED_FIFO
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        std::cerr << "[warn] Could not set SCHED_FIFO (run with sudo for best latency)\n";
    }
#endif
}

int main() {
    // Explicitly zero-initialise all atomics.
    for (auto& a : param_data) a.store(0, std::memory_order_relaxed);

    // 1. Setup Serial
    int serial_fd = setup_serial(SERIAL_PORT);
    if (serial_fd < 0) {
        std::cerr << "Error opening serial port " << SERIAL_PORT << "\n";
        return 1;
    }
    std::cout << "Connected to Serial: " << SERIAL_PORT << "\n";

    // 2. Setup OSC Server Thread
    lo_server_thread st = lo_server_thread_new("4321", error);
    if (!st) {
        std::cerr << "Could not start OSC server on port 4321\n";
        close(serial_fd);
        return 1;
    }

    // 3. Register Parameters
    for (const auto& p : PARAMS) {
        lo_server_thread_add_method(st, p.route.c_str(), "i",
                                    generic_handler,
                                    reinterpret_cast<void*>(
                                        static_cast<intptr_t>(p.index)));
    }

    lo_server_thread_start(st);
    std::cout << "OSC server started on port 4321\n";

    // 4. Register with RNBO
    lo_address target = lo_address_new("127.0.0.1", "1234");
    lo_send(target, "/rnbo/listeners/add", "s", "127.0.0.1:4321");
    lo_address_free(target);   // free immediately; we don't reuse it

    // 5. Elevate main thread priority & pin to a core
    configure_realtime_thread();

    // 6. Transmission buffer — kept on the stack for cache locality
    uint8_t tx_buffer[16];
    tx_buffer[0]  = 254; // Start byte
    tx_buffer[15] = 255; // End byte

    const auto frame_duration =
        std::chrono::nanoseconds(1'000'000'000 / FPS);

    // Seed next_frame from now so the very first sleep is correct.
    auto next_frame = std::chrono::steady_clock::now() + frame_duration;

    int write_err_count = 0;

    std::cout << "Entering main loop at " << FPS << " FPS...\n";

    while (true) {
        // --- Snapshot all 14 atomics in one tight loop ---
        // The compiler will typically unroll this at -O2/-O3.
        for (int i = 0; i < 14; ++i) {
            tx_buffer[i + 1] =
                param_data[i].load(std::memory_order_relaxed);
        }

        // --- Write to serial (non-blocking; EAGAIN is not an error) ---
        if (write(serial_fd, tx_buffer, 16) < 0 && errno != EAGAIN) {
            if (++write_err_count % WRITE_ERR_REPORT_INTERVAL == 0) {
                std::cerr << "[warn] Serial write failed "
                          << write_err_count << " times\n";
            }
        }

        // --- Timing: sleep until the next frame deadline ---
        // If we've somehow fallen behind (e.g. after a long system hiccup),
        // clamp next_frame to now() to avoid a catch-up burst of frames.
        const auto now = std::chrono::steady_clock::now();
        if (next_frame < now) {
            next_frame = now + frame_duration;
        } else {
            next_frame += frame_duration;
        }
        std::this_thread::sleep_until(next_frame);
    }

    // Cleanup (unreachable in normal operation; here for correctness)
    lo_server_thread_free(st);
    close(serial_fd);
    return 0;
}