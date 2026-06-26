
//
//   vibecoded with gemini
//

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <array>
#include <fcntl.h> 
#include <termios.h> 
#include <unistd.h>
#include <lo/lo.h>

// --- Configuration ---
const char* SERIAL_PORT = "/dev/ttyACM0";
const speed_t BAUD_RATE = B115200;
const int FPS = 260;

struct ParamConfig {
    int index;
    std::string route;
};

const std::vector<ParamConfig> PARAMS = {
    {0,  "/brightness"}, {1,  "/radius"}, {2,  "/palette"},
    {3,  "/divisionsHi"},{4,  "/divisionsLo"},{5,  "/width"},
    {6,  "/curve"},      {7,  "/rotation"},   {8,  "/fadeIn"},
    {9,  "/fadeOut"},    {10, "/peakPosition"},{11, "/pattern"},
    {12, "/gradientOffset"},{13, "/maskType"}
};

// --- Global State ---
// Lock-free atomic array for the 14 dynamic data bytes only.
// Initialized to 0 by default.
std::array<std::atomic<uint8_t>, 14> param_data{};

// --- OSC Callback Handler ---
int generic_handler(const char *path, const char *types, lo_arg **argv,
                    int argc, void *data, void *user_data) {
    
    int index = (int)(intptr_t)user_data;
    
    if (index >= 0 && index < 14) {
        // Lock-free, low-overhead atomic write
        param_data[index].store(static_cast<uint8_t>(argv[0]->i), std::memory_order_relaxed);
    }
    
    return 0;
}

void error(int num, const char *msg, const char *path) {
    std::cerr << "Liblo Server Error " << num << " in " << (path ? path : "unknown") << ": " << msg << "\n";
}

int setup_serial(const char* port) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) return -1;

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, BAUD_RATE);
    cfsetispeed(&tty, BAUD_RATE);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main() {
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
        return 1;
    }

    // 3. Register Parameters
    for (const auto& p : PARAMS) {
        lo_server_thread_add_method(st, p.route.c_str(), "i", generic_handler, (void*)(intptr_t)p.index);
    }

    lo_server_thread_start(st);
    std::cout << "OSC server started on port 4321\n";

    // 4. Register with RNBO
    lo_address target = lo_address_new("127.0.0.1", "1234");
    lo_send(target, "/rnbo/listeners/add", "s", "127.0.0.1:4321");

    // 5. Setup Local Transmission Buffer
    // Keep this local to avoid cache misses and external access overhead
    uint8_t tx_buffer[16];
    tx_buffer[0] = 254;  // Start Byte
    tx_buffer[15] = 255; // End Byte

    auto next_frame = std::chrono::steady_clock::now();
    const auto frame_duration = std::chrono::nanoseconds(1000000000 / FPS);

    std::cout << "Entering main loop..." << "\n";
    
    try {
        while (true) {
            // Lock-free read: rapidly copy atomic data into the local transmission buffer
            for (int i = 0; i < 14; ++i) {
                tx_buffer[i + 1] = param_data[i].load(std::memory_order_relaxed);
            }

            // Write to serial
            if (write(serial_fd, tx_buffer, 16) < 0) {
                // Fail silently to prevent massive CPU spikes if the serial connection drops.
                // Alternatively, keep a counter here and only print every 260th failure.
            }

            next_frame += frame_duration;
            std::this_thread::sleep_until(next_frame);
        }
    } catch (...) {
        std::cout << "Stopping..." << "\n";
    }

    // Cleanup
    lo_address_free(target);
    lo_server_thread_free(st);
    close(serial_fd);
    return 0;
}