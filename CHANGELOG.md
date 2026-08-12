# Changelog

All notable changes to this project are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-08-12

Initial release.

### Added

- QSPI Low-Level Analyzer for Saleae Logic 2, decoding chip select, serial clock, and up to
  four data lines (IO0–IO3) as CS-framed **Command → Address → Dummy → Data** transactions.
- Per-phase bus width settings (Single / Dual / Quad) for the command, address, and data
  phases, with MSB-first lane assembly (quad nibble = `IO3:IO0`).
- Configurable address length (none / 8 / 16 / 24 / 32 bits) and dummy cycles (0–63,
  covering continuous-read mode cycles).
- Configurable clock polarity and phase (CPOL/CPHA, all four SPI modes) and chip-select
  active level (low or high).
- IO2/IO3 optional when no phase uses quad width, for captures with limited channels.
- Dual-lane capture in single-IO mode (IO0 command/write lane and IO1 response lane
  recorded per byte, like a classic SPI MOSI/MISO pair).
- Error reporting: truncated frames when CS deasserts mid-phase, and a clock-not-idle
  check at CS assert, with error markers and bubbles.
- FrameV2 output (`enable` / `command` / `address` / `dummy` / `data` / `error` /
  `disable`) feeding the Logic 2 data table and Python High-Level Analyzers.
- Waveform bubbles and per-sampled-edge markers, tabular text, and CSV export
  (`Time, Phase, Value, IO1 Value`).
- Simulation data generator producing settings-driven `0xEB` read transactions
  (`CMD 0xEB → ADDR 0x001000 → dummy cycles → DE AD BE EF`), enabling end-to-end
  validation in Logic 2 demo mode with no hardware attached — verified against a real
  Logic 2 session.
- Unit test suite (250k+ assertions) running the real decoder and simulator against an
  in-process mock of the Analyzer SDK runtime: hand-authored golden waveforms from the
  QSPI spec, a 4,320-permutation closed-loop sweep across all setting combinations, and
  truncation/error/streaming edge cases.
- CI for Windows (x64/ARM64), macOS (x64/ARM64), and Linux (x64/ARM64) with tests on
  every platform and an enforced **100% line / 100% branch coverage** gate
  (clang source-based coverage, no exclusions). Tagged commits publish release binaries.
- AnalyzerSDK pinned to a fixed commit for reproducible builds.

### Known limitations

- Phase configuration is static per capture: mixed-opcode traffic with differing shapes
  (e.g. QSPI PSRAM reads with 6 wait cycles vs writes with 0) requires decoding per
  configuration, or two analyzer instances. A per-opcode command table is the planned
  follow-up.
- DDR (double data rate) and octal modes are not supported.

[1.0.0]: https://github.com/stevenpickles/saleae-logic2-qspi-analyzer/releases/tag/v1.0.0
