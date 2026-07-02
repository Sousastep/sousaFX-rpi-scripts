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
#include <semaphore.h>

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
// NOTE: send() does a sendto() syscall. This struct is now only ever touched
// from the worker thread (never from the JACK RT callback), so a syscall here
// is fine — it can no longer cause an xrun.

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

// ─── Lock-free single-producer / single-consumer sample ring ────────────────
// Producer = JACK RT thread (push only: array writes + one atomic store,
// no locks, no syscalls, no allocation -> safe to call from process()).
// Consumer = worker thread (reads by absolute sample index).

class SampleRing {
public:
    void init(int minCapacitySamples) {
        size_t cap = 1;
        while (cap < static_cast<size_t>(minCapacitySamples)) cap <<= 1;
        mMask = cap - 1;
        mBuf.assign(cap, 0.0f);
    }

    // RT-safe: call only from the JACK process callback.
    void push(const float* src, size_t n) {
        uint64_t w = mWrite.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i)
            mBuf[static_cast<size_t>(w + i) & mMask] = src[i];
        mWrite.store(w + n, std::memory_order_release);
    }

    uint64_t writeIndex() const { return mWrite.load(std::memory_order_acquire); }

    float sampleAt(uint64_t absoluteIndex) const {
        return mBuf[static_cast<size_t>(absoluteIndex) & mMask];
    }

private:
    std::vector<float> mBuf;
    size_t mMask{0};
    std::atomic<uint64_t> mWrite{0};
};

// ─── Pitch processor ─────────────────────────────────────────────────────────
// All analysis here now runs on the worker thread only. Nothing in this class
// is ever called from the JACK RT callback anymore.

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

    Config mCfg;

    bool init(const Config& cfg) {
        mCfg = cfg;
        int specSize = cfg.fftSize / 2 + 1;

        mFrame.resize(cfg.winSize);
        mMagnitude.resize(specSize);
        mDescriptors.resize(2);
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

    // Called from the worker thread once `hopEndIndex` samples are available
    // in the ring (i.e. samples [hopEndIndex - winSize, hopEndIndex) exist).
    template<typename Fn>
    void analysisHopFromRing(const SampleRing& ring, uint64_t hopEndIndex, Fn callback) {
        uint64_t start = hopEndIndex - static_cast<uint64_t>(mCfg.winSize);
        for (int j = 0; j < mCfg.winSize; ++j)
            mFrame(j) = static_cast<double>(ring.sampleAt(start + static_cast<uint64_t>(j)));
        analysisHop(callback);
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

// ─── Worker thread: does all the heavy lifting off the RT path ─────────────

static void pitchWorkerLoop(SampleRing* ring, PitchProcessor* processor,
                             UdpSender* udp, const Config* cfg, sem_t* dataReady) {
    uint64_t nextHopEnd = static_cast<uint64_t>(cfg->winSize);
    uint8_t oscBuf[256];

    while (g_running.load(std::memory_order_relaxed)) {
        sem_wait(dataReady);

        uint64_t avail = ring->writeIndex();
        while (nextHopEnd <= avail && g_running.load(std::memory_order_relaxed)) {
            processor->analysisHopFromRing(*ring, nextHopEnd, [&](float freq, float conf) {
                if (conf >= cfg->confThresh) {
                    int len = osc::buildFloatMsg(oscBuf, sizeof(oscBuf), "/pitchfreq", freq);
                    udp->send(oscBuf, len);
                    len = osc::buildFloatMsg(oscBuf, sizeof(oscBuf), "/pitchconf", conf);
                    udp->send(oscBuf, len);
                }
            });
            nextHopEnd += static_cast<uint64_t>(cfg->hopSize);
        }
    }
}

// ─── JACK state (shared between callback and main) ───────────────────────────
// Deliberately minimal: the RT callback only needs the ring buffer and the
// semaphore. No processor, no UDP socket, no OSC buffer in the RT path.

struct JackState {
    jack_client_t* client{nullptr};
    jack_port_t*   inPort{nullptr};
    SampleRing*    ring{nullptr};
    sem_t*         dataReady{nullptr};
};

static int jackProcess(jack_nframes_t nframes, void* arg) {
    auto* s = static_cast<JackState*>(arg);
    const float* in = static_cast<const float*>(
        jack_port_get_buffer(s->inPort, nframes));

    // Cheap, deterministic, no locks/syscalls/allocation: safe for the RT thread.
    s->ring->push(in, static_cast<size_t>(nframes));
    sem_post(s->dataReady);
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

    // --- Pitch processor (runs on the worker thread only) ---
    PitchProcessor processor;
    if (!processor.init(cfg)) {
        std::cerr << "PitchProcessor init failed" << std::endl;
        return 1;
    }

    // --- Sample ring: generous headroom (~32x window) so the worker thread
    //     can lag behind briefly under scheduling pressure without losing data.
    SampleRing ring;
    ring.init(cfg.winSize * 32);

    sem_t dataReady;
    sem_init(&dataReady, 0, 0);

    // --- Worker thread: all FFT/pitch analysis + OSC sends happen here ---
    std::thread worker(pitchWorkerLoop, &ring, &processor, &udp, &cfg, &dataReady);

    // --- JACK wiring ---
    JackState state;
    state.client    = client;
    state.ring      = &ring;
    state.dataReady = &dataReady;

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

    // Main thread just waits; audio capture happens in jackProcess() (cheap),
    // analysis + OSC happens in the worker thread (off the RT path).
    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Wake the worker so it notices g_running == false and exits.
    sem_post(&dataReady);
    worker.join();
    sem_destroy(&dataReady);

    jack_deactivate(client);
    jack_client_close(client);
    return 0;
}