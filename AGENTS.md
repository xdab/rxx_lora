# AGENTS.md

`README.md` is the human front page; this is AI agents'.
Please read the main README text before substantial works to gain basic understanding of the project.

This document is to carry what you cannot get from elsewhere: constraints, gates, purposes, orientation.
Anything mechanical — build targets (`make help`), CLI flags (`bin/file_lora -h`), the source map, the `tools/` inventory,
is easily discoverable, so don't put it here.

Extra READMEs *can* reside in subdirectories, for instance `tools/README.md`.
Those are usually worth reading in case you need something from there, and in the reverse direction: worth keeping up to date.

## Repo architecture

Sources live in `src/`, headers in `include/`, as `X.c`/`X.h` pairs; we call them modules.
There's one binary per entry point (`bin/file_lora`, `bin/rtl_lora`, `bin/rsp_lora`) - each links the shared core
but has its own `main_*.c` (usage, preparation) and `device_*.c` (driving the input: file or device).

Each input kind is a pull-only cf32 producer behind `include/device.h`
(`device_open_*`):

`source.c` buffers the device stream, resamples it to `bw*os`,
and presents windows by absolute sample position;

`receiver.c` is the device-agnostic DETECT > SYNC > payload chain.
Devices shouldn't push — a push front end (librtlsdr's async callback, the SDRplay API's
stream/event callbacks) stays hidden in its `device_*.c` (like in `device_rtl.c`).

`tools/` is the Python toolkit around the receiver: frame/IQ builders and
channel simulators, and the regression gates. `tools/README.md` has a finer index.

`data/` **may** contain real over-the-air Meshtastic captures cut into frames, the
material the regression gate decodes; `data/README.md` has the PHY details.
This is a very large folder, so we keep it uncommitted (it's gitignored, too).

## Regression checks

`python3 tools/regression.py` is the commit gate and needs `bin/file_lora` built
(it doesn't build fresh). Every case interacts with the binary: the Python only builds the
IQ (frame.py) and, for the noise arm, the channel degradation (AWGN).

`tools/regression_perf.py` is its speed twin. Some quality verdicts,
but primarily RTx measurement — a real-time multiplier: seconds of IQ
consumed per second of wall time (above 1 means faster than real time).
Mind the defaults mismatch: the binaries run `-o 2`, the Python tools `--os 8`;
cross-check os/sf/bw whenever they meet.

## Quality gates

### Main project code (C)

- clean `make` under `-Wall -Wextra`; `clang-format -i` on touched files;
- `make tidy` passing;
- `python3 tools/regression.py` fully passing;

### Python

- `--selftest` is passing (if one exists);
- `make fmt-check` (ruff format + check, config in `pyproject.toml`) passing — run `make fmt` first, don't waste time hand formatting.

### Documentation

- no historical narratives (war stories) - docs describe the **current** state of things - let history be handled by Git alone;
- no pointless duplication - if some fact can be read off a `--help` or an `ls`, leave it out.
- every in-repo reference must resolve correctly: file exists, line or search string is in it, description is accurate;
- material is scoped to the appropriate location, e.g. subdirectory details live in their READMEs.

## Constraints

- C logging goes through `src/logging.{c,h}` (`log_trace/debug/info/warn/error`) to stderr.
- Experiment/debug hooks are env-var knobs compiled in only by `make experimental` (guarded by `#if RXX_EXPERIMENTAL`); default builds contain none of that code. Grep the codebase for `RXX_EXPERIMENTAL` for the current inventory.

## Glossary, nomenclature

### LoRa (from "Long Range")

Proprietary, low-power, wide-area network (LPWAN) physical layer radio communication technology patented by Semtech.

It uses a variation of linear frequency modulation to encode information, allowing devices to transmit data over extremely long distances.

Signals are easily demodulated even when they arrive well below the thermal noise floor (negative Signal-to-Noise Ratio).

### CSS / FSCM (Chirp Spread Spectrum / Frequency Shift Chirp Modulation)

The underlying modulation technique where data is encoded using "chirps"—continuous, linear sweeps across a frequency band.

In a raw chirp (unmodulated), the frequency increases or decreases linearly over time. To encode a symbol, the chirp is cyclically shifted in the time domain, which translates to a sudden frequency jump during the sweep.

Provides exceptional resilience against multipath fading, Doppler shifts, and narrow-band in-band interference.

### Bandwidth (BW)

The physical width of the allocated frequency spectrum (f_max - f_min).
Standard sub-GHz LoRa channels typically use 125 kHz, 250 kHz, or 500 kHz.
Doubling the bandwidth halves the symbol duration (faster data rate) but lets in more thermal noise power, reducing receiver sensitivity by 3 dB.

### Spreading Factor (SF)

Defines the number of raw bits encoded per transmitted symbol, typically ranging from SF5 to SF12. There are exactly $2^{SF}$ chips per symbol period (T_s = 2^SF / BW). Legacy transceivers (like the SX127x) support SF6 to SF12, while modern hardware (SX126x, SX1280) extends this down to SF5.

Each step up in SF doubles the symbol time-on-air and increases processing gain, adding approximately 2.5 dB of sensitivity. High SF targets maximum range at the cost of very slow throughput; low SF maximizes throughput and minimizes TX battery drain.

### Coding Rate (CR)

The Forward Error Correction (FEC) overhead setting, expressed mathematically as 4 / (4 + n) where n in {1, 2, 3, 4}.

Common configurations range from **4/5** (minimal overhead, 1 parity bit per 4 data bits) to **4/8** (maximum redundancy, parity bits equal data bits).

Adds cyclic code parity bits to the frame header and payload. Higher CR enables the receiver to reconstruct corrupted data bits caused by burst interference or transient noise, increasing packet error resilience at the expense of extra time-on-air.

### CFO / STO / SFO

**Carrier Frequency Offset** - local oscillator mismatch.
**Sampling Time Offset** - fractional symbol timing alignment.
**Sampling Frequency Offset** - sampling clock drift over time between transmitter and receiver.

Hardware limitations and imperfections cause the received signal to drift from its ideal mathematical structure.

They serve as the primary estimation and compensation targets for the synchronization block. Uncompensated CFO or SFO results in "symbol blur," where energy spills into adjacent FFT bins, deteriorating demodulation performance.

### Dechirp

The core digital signal processing step used to demodulate a LoRa symbol.

The incoming complex baseband I/Q signal is multiplied point-by-point by the complex conjugate of a locally generated, unmodulated base upchirp.

This multiplication strips away the linear frequency sweep, collapsing the frequency-varying chirp into a single, stationary, constant-frequency tone. Taking the Fast Fourier Transform (FFT) of this tone yields a sharp energy peak, where the bin index of the peak directly maps to the decoded symbol value.

### Preamble & SFD (Start of Frame Delimiter)

The deterministic synchronization sequence that opens every standard LoRa frame. It typically consists of a programmable sequence of raw unmodulated **upchirps** (usually 8 or 16), followed by 2 network-identifying **Sync Words**, and concludes with exactly **2.25 inverted downchirps** acting as the SFD.

The continuous upchirps provide the receiver with predictable timing to calculate coarse synchronization and CFO. The abrupt phase reversal of the 2.25 downchirps flags the exact boundary of the frame, telling the receiver precisely when payload symbol parsing must begin.

### Sync Word

(also called network id) A specific 1-byte value sent within the preamble that acts as a filter for incoming packets.

After completing initial preamble alignment, the DSP checks the incoming symbol sequence against the configured Sync Word. If the values do not match, the receiver aborts processing the frame and clears its buffers, preventing the modem from wasting power decoding packets from foreign networks.

### Chip

The smallest time unit of the modulation: the raw chirp is divided into $2^{SF}$ equal slices per symbol period, and a symbol's value is which slice its frequency jump lands in.

Not to be confused with **chirp** — the sweep itself. The chirp is the carrier waveform; chips are the clock ticks it is measured in. One chip spans 1/(bw) seconds.

### os (oversampling)

Samples per chip; the receiver's working rate is bw*os.

### cf32 (Complex Float 32)

A standard binary file format extension used for storing raw, uncompressed complex I/Q data.

Data is written sequentially as pairs of interleaved 32-bit floating-point numbers, representing the In-phase (I) and Quadrature (Q) components, within a standard -1.0 to 1.0 full-scale amplitude.

## Guiding principles

**KISS** - this is a simple tool, every abstraction must earn its place.
Don't overcomplicate things. Clean code beats even super comprehensive docs.

**Method** - work here is measurement-driven; the full set of principles
lives in `docs/method.md`. The ones that bind day-to-day:

- Capture once, query many: collect raw inputs, raw outputs, and per-case
  ground truth in one pass; every later question becomes a lookup.
- Compare variants on identical randomness (paired A/B on common seeds), and
  give a confound an arm that separates it.
- When a result is impossible, suspect the analysis harness before the system
  under test.
- A test that cannot fail measures nothing; every verdict needs an
  independent path to a "no."
