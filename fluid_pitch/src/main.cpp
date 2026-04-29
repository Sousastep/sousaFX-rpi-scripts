#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include <complex>

// JACK
#include <jack/jack.h>

// FLUCOMA core
#include <algorithms/public/YINFFT.hpp>
#include <algorithms/public/CepstrumF0.hpp>
#include <algorithms/public/HPS.hpp>
#include <data/FluidTensor.hpp>
#include <algorithms/public/STFT.hpp>

// OSC via POSIX sockets
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static std::atomic<bool> g_running{true};
static void sighandler(int) { g_running = false; }

// ─── OSC ─────────────────────────────────────────────────────────────────────

namespace osc {
static inline size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

static size_t writeString(uint8_t* buf, size_t offset, const char* s) {
    size_t len = std::strlen(s) + 1;
    size_t padded = pad4(len);
    std::memcpy(buf + offset, s, len);
    std::memset(buf + offset + len, 0, padded - len);
    return padded;
}

static size_t writeFloat(uint8_t* buf, size_t offset, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    buf[offset + 0] = (bits >> 24) & 0xFF;
    buf[offset + 1] = (bits >> 16) & 0xFF;
    buf[offset + 2] = (bits >>  8) & 0xFF;
    buf[offset + 3] = (bits >>  0) & 0xFF;
    return 4;
}

static int buildFloatMsg(uint8_t* buf, size_t bufSize, const char* path, float value) {
    size_t off = 0;
    off += writeString(buf, off, path);
    off += writeString(buf, off, ",f");
    off += writeFloat(buf, off, value);
    return static_cast<int>(off);
}
}

// ─── UDP / OSC sender ────────────────────────────────────────────────────────

struct UdpSender {
    int fd{-1};
    sockaddr_in dest{};

    bool open(const std::string& host, int port) {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        std::memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(static_cast<uint16_t>(port));
        return ::inet_pton(AF_INET, host.c_str(), &dest.sin_addr) == 1;
    }

    void send(const uint8_t* buf, int len) {
        ::sendto(fd, buf, static_cast<size_t>(len), 0,
                 reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    }

    ~UdpSender() { if (fd >= 0) ::close(fd); }
};

// ─── Config ──────────────────────────────────────────────────────────────────

struct Config {
    std::string jackClientName{"pitch-detector"};
    std::string jackPort{"system:capture_1"}; // source port to auto-connect
    unsigned    sampleRate{0};  // filled in at runtime from JACK
    int         fftSize{1024};
    int         hopSize{512};
    int         winSize{1024};
    int         algorithm{2}; // 0=Cepstrum, 1=HPS, 2=YinFFT
    int         unit{0};      // 0=Hz, 1=MIDI
    float       minFreq{40.f};
    float       maxFreq{800.f};
    std::string oscHost{"127.0.0.1"};
    int         oscPort{1234};
    float       confThresh{0.f};
};

static Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (i + 1 >= argc) continue;
        std::string val = argv[++i];
        if      (key == "--name")       cfg.jackClientName = val;
        else if (key == "--jackport")   cfg.jackPort       = val;
        else if (key == "--fftsize")    cfg.fftSize        = std::stoi(val);
        else if (key == "--hopsize")    cfg.hopSize        = std::stoi(val);
        else if (key == "--winsize")    cfg.winSize        = std::stoi(val);
        else if (key == "--algorithm")  cfg.algorithm      = std::stoi(val);
        else if (key == "--unit")       cfg.unit           = std::stoi(val);
        else if (key == "--minfreq")    cfg.minFreq        = std::stof(val);
        else if (key == "--maxfreq")    cfg.maxFreq        = std::stof(val);
        else if (key == "--host")       cfg.oscHost        = val;
        else if (key == "--port")       cfg.oscPort        = std::stoi(val);
        else if (key == "--confthresh") cfg.confThresh     = std::stof(val);
    }
    return cfg;
}

// ─── Pitch processor ─────────────────────────────────────────────────────────

struct PitchProcessor {
    using RealTensor = fluid::FluidTensor<double, 1>;

    std::unique_ptr<fluid::algorithm::STFT>       mSTFT;
    std::unique_ptr<fluid::algorithm::YINFFT>     mYinFFT;
    std::unique_ptr<fluid::algorithm::CepstrumF0> mCepstrum;
    std::unique_ptr<fluid::algorithm::HPS>        mHPS;

    RealTensor mFrame;
    RealTensor mMagnitude;
    RealTensor mDescriptors;

    std::vector<std::complex<double>> mSpecBuf;
    std::vector<double> mRing;
    int mRingWrite{0};
    int mHopCount{0};

    Config mCfg;

    bool init(const Config& cfg) {
        mCfg = cfg;
        int specSize = cfg.fftSize / 2 + 1;

        mFrame.resize(cfg.winSize);
        mMagnitude.resize(specSize);
        mDescriptors.resize(2);
        mRing.assign(cfg.winSize, 0.0);
        mSpecBuf.resize(specSize);

        auto& alloc = fluid::FluidDefaultAllocator();

        mSTFT = std::make_unique<fluid::algorithm::STFT>(cfg.winSize, cfg.fftSize, cfg.hopSize, 0, alloc);

        switch (cfg.algorithm) {
            case 0: mCepstrum = std::make_unique<fluid::algorithm::CepstrumF0>(cfg.fftSize, alloc); break;
            case 1: mHPS = std::make_unique<fluid::algorithm::HPS>(); break;
            default: mYinFFT = std::make_unique<fluid::algorithm::YINFFT>(specSize, alloc); break;
        }
        return true;
    }

    template<typename Fn>
    void process(const float* samples, int n, Fn callback) {
        for (int i = 0; i < n; ++i) {
            mRing[mRingWrite] = static_cast<double>(samples[i]);
            mRingWrite = (mRingWrite + 1) % mCfg.winSize;
            if (++mHopCount >= mCfg.hopSize) {
                mHopCount = 0;
                for (int j = 0; j < mCfg.winSize; ++j)
                    mFrame(j) = mRing[(mRingWrite + j) % mCfg.winSize];
                analysisHop(callback);
            }
        }
    }

private:
    template<typename Fn>
    void analysisHop(Fn callback) {
        int specSize = static_cast<int>(mSpecBuf.size());

        fluid::FluidTensorView<double, 1> frameView = mFrame;
        fluid::FluidTensorView<std::complex<double>, 1> specView{mSpecBuf.data(), 0, (fluid::index)specSize};

        mSTFT->processFrame(frameView, specView);

        for (int k = 0; k < specSize; ++k)
            mMagnitude(k) = std::abs(specView(k));

        auto& alloc = fluid::FluidDefaultAllocator();
        fluid::FluidTensorView<double, 1> out = mDescriptors;

        if (mCfg.algorithm == 0) {
            mCepstrum->processFrame(mMagnitude, out, mCfg.minFreq, mCfg.maxFreq, mCfg.sampleRate, alloc);
        } else if (mCfg.algorithm == 1) {
            mHPS->processFrame(mMagnitude, out, 4, mCfg.minFreq, mCfg.maxFreq, mCfg.sampleRate, alloc);
        } else {
            mYinFFT->processFrame(mMagnitude, out, mCfg.minFreq, mCfg.maxFreq, mCfg.sampleRate, alloc);
        }

        double pitch = out(0);
        double confidence = out(1);

        if (mCfg.unit == 1 && pitch > 0.0)
            pitch = 69.0 + 12.0 * std::log2(pitch / 440.0);

        callback(static_cast<float>(pitch), static_cast<float>(confidence));
    }
};

// ─── JACK state (shared between callback and main) ───────────────────────────

struct JackState {
    jack_client_t*  client{nullptr};
    jack_port_t*    inPort{nullptr};
    PitchProcessor* processor{nullptr};
    UdpSender*      udp{nullptr};
    Config*         cfg{nullptr};
    uint8_t         oscBuf[256]{};
    unsigned        sampleRate{0};
};

static int jackProcess(jack_nframes_t nframes, void* arg) {
    auto* s = static_cast<JackState*>(arg);
    const float* in = static_cast<const float*>(
        jack_port_get_buffer(s->inPort, nframes));

    s->processor->process(in, static_cast<int>(nframes), [&](float freq, float conf) {
        if (conf >= s->cfg->confThresh) {
            int len;
            len = osc::buildFloatMsg(s->oscBuf, sizeof(s->oscBuf),
                                     "/rnbo/inst/0/params/pitchfreq", freq);
            s->udp->send(s->oscBuf, len);
            len = osc::buildFloatMsg(s->oscBuf, sizeof(s->oscBuf),
                                     "/rnbo/inst/0/params/pitchconf", conf);
            s->udp->send(s->oscBuf, len);
        }
    });
    return 0;
}

static void jackShutdown(void*) {
    std::cerr << "[JACK] Server shut down, exiting." << std::endl;
    g_running = false;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Config cfg = parseArgs(argc, argv);
    signal(SIGINT,  sighandler);
    signal(SIGTERM, sighandler);

    // --- OSC ---
    UdpSender udp;
    if (!udp.open(cfg.oscHost, cfg.oscPort)) {
        std::cerr << "Failed to open UDP socket" << std::endl;
        return 1;
    }

    // --- JACK client ---
    jack_status_t status;
    jack_client_t* client = jack_client_open(cfg.jackClientName.c_str(),
                                             JackNullOption, &status);
    if (!client) {
        std::cerr << "jack_client_open failed, status=0x" << std::hex << status << std::endl;
        return 1;
    }

    unsigned sampleRate = jack_get_sample_rate(client);
    cfg.sampleRate = sampleRate; // propagate to processor

    // --- Pitch processor ---
    PitchProcessor processor;
    if (!processor.init(cfg)) {
        std::cerr << "PitchProcessor init failed" << std::endl;
        return 1;
    }

    // --- JACK wiring ---
    JackState state;
    state.client    = client;
    state.processor = &processor;
    state.udp       = &udp;
    state.cfg       = &cfg;
    state.sampleRate = sampleRate;

    state.inPort = jack_port_register(client, "in",
                                      JACK_DEFAULT_AUDIO_TYPE,
                                      JackPortIsInput, 0);
    if (!state.inPort) {
        std::cerr << "Could not register JACK port" << std::endl;
        return 1;
    }

    jack_set_process_callback(client, jackProcess, &state);
    jack_on_shutdown(client, jackShutdown, nullptr);

    if (jack_activate(client) != 0) {
        std::cerr << "Cannot activate JACK client" << std::endl;
        return 1;
    }

    // Auto-connect to the requested source port (if specified)
    if (!cfg.jackPort.empty()) {
        std::string destPort = std::string(jack_get_client_name(client)) + ":in";
        if (jack_connect(client, cfg.jackPort.c_str(), destPort.c_str()) != 0)
            std::cerr << "[JACK] Warning: could not auto-connect to "
                      << cfg.jackPort << " — connect manually." << std::endl;
    }

    std::cout << "[RUNNING] JACK client '" << jack_get_client_name(client)
              << "' @ " << sampleRate << " Hz  →  OSC "
              << cfg.oscHost << ":" << cfg.oscPort << std::endl;

    // Main thread just waits; audio processing happens in jackProcess()
    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    jack_deactivate(client);
    jack_client_close(client);
    return 0;
}
