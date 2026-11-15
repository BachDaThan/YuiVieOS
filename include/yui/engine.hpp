// yui_engine — the numeric core of Tầng 4 (`yui.cpp`).
//
// Scope here is the *portable, testable* half of the engine: ISA detection,
// q4/t2 block quantisation, and the elastic k/W autotuner.  The heavyweight
// expert weights are supplied at runtime by the optional y-app/adaptive-moe
// bundle; nothing in this file hard-codes a model.
#ifndef YUI_ENGINE_HPP
#define YUI_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yui::engine {

// ------------------------------------------------------------ ISA support --
struct SimdInfo {
  std::string arch;      // "x86_64" | "aarch64" | "riscv64" | other
  std::string best;      // best available vector ISA: "avx512f" | "avx2" | "neon" | "rvv" | "scalar"
  bool avx2 = false;
  bool avx512 = false;
  bool fma = false;
  bool neon = false;
  bool rvv = false;
  unsigned vector_bits = 64;  // best-case SIMD lane width
  std::string cpu_name;       // best-effort model string for the profiler UI
};
SimdInfo detect_simd();

// ------------------------------------------------- q4 block quantisation ---
// Block layout (32 values per block, 20 bytes on disk):
//   float  d      scale (max/7, symmetric)
//   uint8  qs[16] two 4-bit signed values per byte: low nibble = value 2i,
//                 high nibble = value 2i+1, both biased by 8.
// This mirrors the ggml q4_0 family closely enough that a converter can map
// between them without touching arithmetic.
struct Q4Block {
  float d;
  uint8_t qs[16];
};
constexpr size_t kQ4BlockValues = 32;

// n must be a multiple of kQ4BlockValues (padded with zeros otherwise).
void quantize_q4(const float* x, size_t n, std::vector<Q4Block>& out);
void dequantize_q4(const std::vector<Q4Block>& blocks, float* out, size_t n);
// Dot product of quantised weights with fp32 activations; exactly
// sum(round(scale*q) * y) so scalar and SIMD paths agree bit-for-bit.
float dot_q4(const Q4Block* blocks, const float* y, size_t n);
// Which kernel dot_q4 dispatched to ("scalar" or "avx2"), for the status page.
std::string dot_kernel_name();

// t2: 2-bit (ternary-ish) block, 32 values in 8 bytes + scale.  Values are
// {-1, 0, +1} scaled, chosen by thresholding; used for the "máy yếu" profile
// where RAM bandwidth, not compute, is the bottleneck.
struct T2Block {
  float d;
  uint8_t qs[8];  // 4 x 2-bit codes per byte
};
void quantize_t2(const float* x, size_t n, std::vector<T2Block>& out);
void dequantize_t2(const std::vector<T2Block>& blocks, float* out, size_t n);

// ------------------------------------------------ elastic k / W autotune --
// k = how many experts/graph neighbours to activate, W = context window in
// token-ish units.  The tuner trades quality for latency and thermals: it is
// the piece that makes the same binary usable on a 2 GB netbook and a
// workstation.
struct TuneInput {
  double ram_gb = 8.0;
  double temperature_c = 45.0;
  bool on_battery = false;
  bool low_power_mode = false;
  int cpu_threads = 4;
  bool dedicated_gpu = false;
};
struct TuneResult {
  int k = 4;
  int window = 2048;
  int threads = 4;
  std::string profile;  // "may-yeu" | "can-bang" | "may-manh"
  std::string reason;   // Vietnamese explanation shown in the shell
};
TuneResult auto_tune(const TuneInput& in);

}  // namespace yui::engine

#endif  // YUI_ENGINE_HPP
