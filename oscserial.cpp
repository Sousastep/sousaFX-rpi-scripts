
//
//   https://gemini.google.com/share/03fb478f2064
//

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <fcntl.h> 
#include <termios.h> 
#include <unistd.h>
#include <lo/lo.h>

// --- Configuration ---
const char* SERIAL_PORT = "/dev/ttyACM0";
const speed_t BAUD_RATE = B115200;
const int FPS = 260;

// Parameter structure to mirror the Python dictionary
struct ParamConfig {
    int index;
    std::string route;
};

const std::vector<ParamConfig> PARAMS = {
    {0,  "/rnbo/inst/1/messages/out/brightness"},
    {1,  "/rnbo/inst/1/messages/out/radius"},
    {2,  "/rnbo/inst/1/messages/out/palette"},
    {3,  "/rnbo/inst/1/messages/out/divisionsHi"},
    {4,  "/rnbo/inst/1/messages/out/divisionsLo"},
    {5,  "/rnbo/inst/1/messages/out/width"},
    {6,  "/rnbo/inst/1/messages/out/curve"},
    {7,  "/rnbo/inst/1/messages/out/rotation"},
    {8,  "/rnbo/inst/1/messages/out/fadeIn"},
    {9,  "/rnbo/inst/1/messages/out/fadeOut"},
    {10, "/rnbo/inst/1/messages/out/peakPosition"},
    {11, "/rnbo/inst/1/messages/out/pattern"},
    {12, "/rnbo/inst/1/messages/out/gradientOffset"}
};

// --- Global State ---
// Buffer: [Start Byte] + [13 Data Bytes] + [End Byte] = 15 total
unsigned char tx_buffer[15] = {254, 0,0,0,0,0,0,0,0,0,0,0,0,0, 255};
std::mutex buffer_mutex;

// --- OSC Callback Handler ---
int generic_handler(const char *path, const char *types, lo_arg **argv,
                    int argc, void *data, void *user_data) {
    
    // Convert user_data pointer back to the integer index
    int index = (int)(intptr_t)user_data;
    
    // Update buffer thread-safely
    std::lock_guard<std::mutex> lock(buffer_mutex);
    if (index >= 0 && index < 13) {
        tx_buffer[index + 1] = (unsigned char)argv[0]->i;
    }
    
    return 0;
}

void error(int num, const char *msg, const char *path) {
    std::cerr << "Liblo Server Error " << num << " in " << (path ? path : "unknown") << ": " << msg << std::endl;
}

// Refined Serial Setup: Now returns -1 on failure instead of exiting
int setup_serial(const char* port) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) return -1;

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

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
        std::cerr << "Error opening serial port " << SERIAL_PORT << std::endl;
        return 1;
    }
    std::cout << "Connected to Serial: " << SERIAL_PORT << std::endl;

    // 2. Setup OSC Server Thread
    lo_server_thread st = lo_server_thread_new("4321", error);
    if (!st) {
        std::cerr << "Could not start OSC server on port 4321" << std::endl;
        return 1;
    }

    // 3. Register Parameters from the vector loop
    for (const auto& p : PARAMS) {
        lo_server_thread_add_method(st, p.route.c_str(), "i", generic_handler, (void*)(intptr_t)p.index);
    }

    lo_server_thread_start(st);
    std::cout << "OSC server started on port 4321" << std::endl;

    // 4. Register with RNBO
    lo_address target = lo_address_new("127.0.0.1", "1234");
    lo_send(target, "/rnbo/listeners/add", "s", "127.0.0.1:4321");

    // 5. Main Loop (Serial Transmission at 260 FPS)
    auto next_frame = std::chrono::steady_clock::now();
    auto frame_duration = std::chrono::nanoseconds(1000000000 / FPS);

    std::cout << "Entering main loop..." << std::endl;
    
    try {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (write(serial_fd, tx_buffer, 15) < 0) {
                    std::cerr << "Serial write error" << std::endl;
                }
            }

            next_frame += frame_duration;
            std::this_thread::sleep_until(next_frame);
        }
    } catch (...) {
        std::cout << "Stopping..." << std::endl;
    }

    // Cleanup
    lo_server_thread_free(st);
    close(serial_fd);
    return 0;
}