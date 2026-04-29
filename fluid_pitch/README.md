# flucoma_pitch_osc

A standalone C++ program for **Raspberry Pi 5** that:

1. Captures mono audio as a Jack client.
2. Runs FLUCOMA's pitch estimation (YinFFT / Cepstrum / HPS) on each hop.
3. Streams **frequency** and **confidence** values to a **RNBO patch** via OSC/UDP.

---

### Install

```bash
sudo apt update
sudo apt install libasound2-dev
sudo apt install libjack-jackd2-dev
sudo apt install git
```

---

## Modify

RNBO patchers are numbered ( 0-indexed ) in the order that they're uploaded to the rpi. 

In main.cpp, the number in "/rnbo/inst/0/params/" should be modified to match the number of the patcher that the OSC data will be sent to.

---

## Build

```bash
# 1. Clone
git clone https://github.com/your-repo/flucoma_pitch_osc.git
cd flucoma_pitch_osc

# 2. Configure — FetchContent will download flucoma-core, Eigen, HISSTools
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Build (takes a few minutes on first run due to downloads)
cmake --build build -j$(nproc)
```

---

## .asoundrc

The program works without an .asoundrc file if rnboscquery is disabled (`sudo service rnbooscquery stop`), and we use the device flag "plughw:AMS24". However, we need the program to work while rnboscquery is enabled.

using "--device jack" with the following asoundrc file does not work properly due to creating a stereo connection instead of mono for some reason:

```bash
pcm.jack {
    type jack
    playback_ports {
        0 system:playback_1
    }
    capture_ports {
        0 system:capture_1
    }
}
```

```bash
pi@c74rpi:~ $ jack_lsp -c
system:capture_1
   flucoma_pitch_osc.C.31911.0:in_000
system:capture_2
   flucoma_pitch_osc.C.31911.0:in_001
system:playback_1
```

Using no device flag with the following asoundrc file does indeed work properly:

```bash
pcm.!default {
    type jack
    playback_ports {
        0 system:playback_1
    }
    capture_ports {
        0 system:capture_1
    }
}

ctl.!default {
    type hw
    card AMS24
}
```

```bash
pi@c74rpi:~ $ jack_lsp -c
system:capture_1
   flucoma_pitch_osc.C.31919.0:in_000
system:capture_2
system:playback_1
```


## Run

```bash
./build/flucoma_pitch_osc --rate 48000 --blocksize 256 --fftsize 4096 --winsize 4096 --minFreq 20 --maxFreq 400 --hopsize 4096
```

### All flags

| Flag           | Default     | Description                                         |
|----------------|-------------|-----------------------------------------------------|
| `--device`     | `default`   | ALSA capture device (`hw:0,0`, `plughw:1,0`, …)    |
| `--rate`       | `44100`     | Sample rate in Hz                                   |
| `--blocksize`  | `512`       | ALSA period size (= FLUCOMA host vector size)       |
| `--fftsize`    | `1024`      | FFT transform size                                  |
| `--hopsize`    | `512`       | FFT hop size (analysis rate = rate / hopsize)       |
| `--winsize`    | `1024`      | Analysis window size                                |
| `--algorithm`  | `2`         | `0` Cepstrum · `1` HPS · `2` YinFFT (recommended)  |
| `--unit`       | `0`         | `0` Hz · `1` MIDI note number                      |
| `--minfreq`    | `20.0`      | Lower frequency bound (Hz)                          |
| `--maxfreq`    | `10000.0`   | Upper frequency bound (Hz)                          |
| `--host`       | `127.0.0.1` | OSC/UDP destination IP                              |
| `--port`       | `7400`      | OSC/UDP destination port                            |
| `--confthresh` | `0.0`       | Don't send OSC below this confidence level          |

---

## Algorithms

| Value | Name   | Notes                                                                 |
|-------|--------|-----------------------------------------------------------------------|
| `0`   | Cepstrum | Fast; uses second peak of cepstrum. Less accurate.                  |
| `1`   | HPS      | Harmonic Product Spectrum. Good for instruments with clear harmonics.|
| `2`   | YinFFT   | **Default.** Frequency-domain YIN. Most accurate, most CPU.         |

---

## Tuning for Raspberry Pi 5

- **blocksize 256** gives ~5.8 ms latency but higher CPU load.
- **blocksize 512** (default) gives ~11.6 ms — a good balance.
- YinFFT at 44100 Hz with `fftsize=1024` uses <5% of one Cortex-A76 core.
- Use `--confthresh 0.6` or higher to avoid sending spurious OSC during silences.

---

## License

BSD-3-Clause (matching flucoma-core)
