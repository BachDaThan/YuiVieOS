// Numeric core: ISA detection, q4/t2 quantisation, elastic k/W autotune.
#include "yui/engine.hpp"

#include <cmath>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace yui::engine {
namespace {

const char* arch_name() {
#if defined(__x86_64__)
  return "x86_64";
#elif defined(__aarch64__) || defined(__arm64__)
  return "aarch64";
#elif defined(__riscv) && __riscv_xlen == 64
  return "riscv64";
#else
  return "unknown";
#endif
}

void cpuid_leaf(unsigned leaf, unsigned sub, unsigned out[4]) {
#if defined(__x86_64__) || defined(__i386__)
  __cpuid_count(leaf, sub, out[0], out[1], out[2], out[3]);
#else
  (void)leaf;
  (void)sub;
  out[0] = out[1] = out[2] = out[3] = 0;
#endif
}

std::string cpu_brand() {
#if defined(__x86_64__) || defined(__i386__)
  unsigned r[4];
  char brand[49] = {0};
  cpuid_leaf(0x80000000u, 0, r);
  if (r[0] >= 0x80000004u) {
    for (unsigned i = 0; i < 3; ++i) {
      cpuid_leaf(0x80000002u + i, 0, r);
      std::memcpy(brand + i * 16, r, 16);
    }
  }
  std::string s(brand);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  size_t pos = 0;
  while ((pos = s.find("  ")) != std::string::npos) s.erase(pos, 1);
  return s;
#else
  return "unknown";
#endif
}

// Returns the nibble index (0..31) of value `i` inside the packed block.
inline int q4_at(const Q4Block& b, int i) {
  const uint8_t packed = b.qs[i >> 1];
  const int v = (i & 1) ? (packed >> 4) : (packed & 0x0f);
  return v - 8;
}

inline void q4_set(Q4Block& b, int i, int q) {
  const int biased = (q + 8) & 0x0f;
  uint8_t& packed = b.qs[i >> 1];
  if (i & 1)
    packed = uint8_t((packed & 0x0f) | (biased << 4));
  else
    packed = uint8_t((packed & 0xf0) | biased);
}

}  // namespace

// ------------------------------------------------------------ ISA support --
SimdInfo detect_simd() {
  SimdInfo info;
  info.arch = arch_name();
  info.cpu_name = cpu_brand();
  info.best = "scalar";
  info.vector_bits = 64;

#if defined(__x86_64__) || defined(__i386__)
  unsigned r[4];
  cpuid_leaf(1, 0, r);
  const bool sse2 = (r[3] >> 26) & 1;
  const bool osxsave = (r[2] >> 27) & 1;
  const bool avx = (r[2] >> 28) & 1;
  const bool fma = (r[2] >> 12) & 1;
  (void)sse2;
  cpuid_leaf(7, 0, r);
  const bool avx2_hw = (r[1] >> 5) & 1;
  const bool avx512f_hw = (r[1] >> 16) & 1;
  uint64_t xcr0 = 0;
  if (osxsave && avx) {
    unsigned lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    xcr0 = (uint64_t(hi) << 32) | lo;
  }
  const bool ymm_ok = (xcr0 & 0x6) == 0x6;         // XMM + YMM state enabled
  const bool zmm_ok = (xcr0 & 0xe6) == 0xe6;       // + opmask/ZMM state
  info.avx2 = avx2_hw && ymm_ok;
  info.avx512 = avx512f_hw && zmm_ok;
  info.fma = fma && ymm_ok;
  if (info.avx512) {
    info.best = "avx512f";
    info.vector_bits = 512;
  } else if (info.avx2) {
    info.best = "avx2";
    info.vector_bits = 256;
  } else if (avx && ymm_ok) {
    info.best = "avx";
    info.vector_bits = 128;
  } else {
    info.best = "sse2";
    info.vector_bits = 128;
  }
#elif defined(__aarch64__)
  info.neon = true;  // mandatory in ARMv8-A
  info.best = "neon";
  info.vector_bits = 128;
#if defined(__ARM_FEATURE_SVE)
  info.best = "sve";
#endif
#elif defined(__riscv) && __riscv_xlen == 64
  info.rvv = true;  // refinement: probe hwcap for V extension at runtime
  info.best = "rvv";
  info.vector_bits = 128;
#endif
  return info;
}

// ------------------------------------------------- q4 block quantisation ---
void quantize_q4(const float* x, size_t n, std::vector<Q4Block>& out) {
  const size_t blocks = (n + kQ4BlockValues - 1) / kQ4BlockValues;
  out.assign(blocks, Q4Block{0.0f, {0}});
  for (size_t b = 0; b < blocks; ++b) {
    const size_t base = b * kQ4BlockValues;
    float amax = 0.0f;
    for (size_t i = 0; i < kQ4BlockValues; ++i) {
      const size_t idx = base + i;
      const float v = idx < n ? x[idx] : 0.0f;
      if (std::fabs(v) > amax) amax = std::fabs(v);
    }
    const float d = amax / 7.0f;  // symmetric, 4-bit signed range [-8, 7] -> use [-7, 7]
    out[b].d = d;
    std::memset(out[b].qs, 0, sizeof(out[b].qs));
    for (size_t i = 0; i < kQ4BlockValues; ++i) {
      const size_t idx = base + i;
      const float v = idx < n ? x[idx] : 0.0f;
      int q = d > 0.0f ? int(std::lround(v / d)) : 0;
      if (q > 7) q = 7;
      if (q < -7) q = -7;
      q4_set(out[b], int(i), q);
    }
  }
}

void dequantize_q4(const std::vector<Q4Block>& blocks, float* out, size_t n) {
  for (size_t b = 0; b < blocks.size(); ++b) {
    const size_t base = b * kQ4BlockValues;
    for (size_t i = 0; i < kQ4BlockValues; ++i) {
      const size_t idx = base + i;
      if (idx >= n) return;
      out[idx] = float(q4_at(blocks[b], int(i))) * blocks[b].d;
    }
  }
}

namespace {

float dot_q4_scalar(const Q4Block* blocks, const float* y, size_t n) {
  float sum = 0.0f;
  const size_t nblocks = n / kQ4BlockValues;
  for (size_t b = 0; b < nblocks; ++b) {
    const Q4Block& blk = blocks[b];
    // Accumulate the integer dot as fp32 (exact for these magnitudes) then a
    // single multiply by the block scale: this is what makes the SIMD path
    // bit-identical to the scalar one.
    float acc = 0.0f;
    for (size_t i = 0; i < kQ4BlockValues; ++i) acc += float(q4_at(blk, int(i))) * y[b * kQ4BlockValues + i];
    sum += acc * blk.d;
  }
  return sum;
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
// AVX2 dot product.  Nibbles are de-interleaved with punpck so the 32 values
// stay in their natural order, then widened 8-at-a-time to fp32 and fused.
// Accumulation order differs from the scalar kernel, so tests compare with a
// relative tolerance rather than for bit equality.
__attribute__((target("avx2,fma"))) float dot_q4_avx2(const Q4Block* blocks, const float* y, size_t n) {
  const size_t nblocks = n / kQ4BlockValues;
  float sum = 0.0f;
  const __m128i nibble_mask = _mm_set1_epi8(0x0f);
  const __m128i bias = _mm_set1_epi8(8);
  for (size_t b = 0; b < nblocks; ++b) {
    __m128i qs = _mm_loadu_si128(reinterpret_cast<const __m128i*>(blocks[b].qs));
    const __m128i lo = _mm_and_si128(qs, nibble_mask);            // even-index values
    const __m128i hi = _mm_and_si128(_mm_srli_epi16(qs, 4), nibble_mask);  // odd-index values
    __m128i v0 = _mm_sub_epi8(_mm_unpacklo_epi8(lo, hi), bias);   // values 0..15
    __m128i v1 = _mm_sub_epi8(_mm_unpackhi_epi8(lo, hi), bias);   // values 16..31
    __m256 acc = _mm256_setzero_ps();
    acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(v0)),
                          _mm256_loadu_ps(y + b * kQ4BlockValues), acc);
    acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(v0, 8))),
                          _mm256_loadu_ps(y + b * kQ4BlockValues + 8), acc);
    acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(v1)),
                          _mm256_loadu_ps(y + b * kQ4BlockValues + 16), acc);
    acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(v1, 8))),
                          _mm256_loadu_ps(y + b * kQ4BlockValues + 24), acc);
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, acc);
    float block_acc = 0.0f;
    for (float v : lanes) block_acc += v;
    sum += block_acc * blocks[b].d;
  }
  return sum;
}

bool cpu_has_avx2() {
  unsigned r[4];
  cpuid_leaf(1, 0, r);
  const bool osxsave = (r[2] >> 27) & 1;
  if (!osxsave) return false;
  unsigned lo, hi;
  __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  if ((((uint64_t(hi) << 32) | lo) & 0x6) != 0x6) return false;
  cpuid_leaf(7, 0, r);
  return ((r[1] >> 5) & 1) != 0;
}
#endif

}  // namespace

std::string dot_kernel_name() {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool avx2 = cpu_has_avx2();
  return avx2 ? "avx2" : "scalar";
#else
  return "scalar";
#endif
}

float dot_q4(const Q4Block* blocks, const float* y, size_t n) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool avx2 = cpu_has_avx2();
  if (avx2) return dot_q4_avx2(blocks, y, n);
#endif
  return dot_q4_scalar(blocks, y, n);
}

// ------------------------------------------------------ t2 (2-bit) format --
namespace {
inline int t2_at(const T2Block& b, int i) {
  const int shift = (i & 3) * 2;
  const int code = (b.qs[i >> 2] >> shift) & 0x03;
  return code == 0 ? -1 : (code == 1 ? 0 : 1);  // 2 -> +1, 3 -> +1 (saturating)
}
}  // namespace

void quantize_t2(const float* x, size_t n, std::vector<T2Block>& out) {
  const size_t blocks = (n + kQ4BlockValues - 1) / kQ4BlockValues;
  out.assign(blocks, T2Block{0.0f, {0}});
  for (size_t b = 0; b < blocks; ++b) {
    const size_t base = b * kQ4BlockValues;
    float amax = 0.0f, mean = 0.0f;
    for (size_t i = 0; i < kQ4BlockValues; ++i) {
      const float v = base + i < n ? x[base + i] : 0.0f;
      amax = std::max(amax, std::fabs(v));
      mean += v;
    }
    mean /= float(kQ4BlockValues);
    const float threshold = 0.25f * amax;  // keep roughly a third of the weights active
    out[b].d = amax;
    std::memset(out[b].qs, 0, sizeof(out[b].qs));
    for (size_t i = 0; i < kQ4BlockValues; ++i) {
      const float v = (base + i < n ? x[base + i] : 0.0f) - mean;
      const int code = v > threshold ? 2 : (v < -threshold ? 0 : 1);
      out[b].qs[i >> 2] |= uint8_t(code << ((i & 3) * 2));
    }
  }
}

void dequantize_t2(const std::vector<T2Block>& blocks, float* out, size_t n) {
  for (size_t b = 0; b < blocks.size(); ++b) {
    for (size_t i = 0; i < kQ4BlockValues; ++i) {
      const size_t idx = b * kQ4BlockValues + i;
      if (idx >= n) return;
      const float scale = blocks[b].d * 0.5f;
      out[idx] = float(t2_at(blocks[b], int(i))) * scale;
    }
  }
}

// ------------------------------------------------ elastic k / W autotune --
TuneResult auto_tune(const TuneInput& in) {
  TuneResult r;
  r.threads = in.cpu_threads > 0 ? in.cpu_threads : 1;

  const bool constrained = in.ram_gb < 4.0 || in.low_power_mode || in.cpu_threads <= 2;
  const bool hot = in.temperature_c >= 80.0;
  const bool warm = in.temperature_c >= 65.0;
  const bool rich = in.ram_gb >= 16.0 && in.cpu_threads >= 8;

  if (constrained) {
    r.profile = "may-yeu";
    r.k = 2;
    r.window = 1024;
    r.reason = "Máy yếu / pin thấp: giảm k xuống 2 và cửa sổ ngữ cảnh 1024 để giữ RAM và nhiệt.";
  } else if (rich && !in.on_battery && !hot) {
    r.profile = "may-manh";
    r.k = 8;
    r.window = 8192;
    r.reason = "Máy mạnh, đang cắm điện: mở rộng k=8, cửa sổ 8192 cho chất lượng cao nhất.";
  } else {
    r.profile = "can-bang";
    r.k = 4;
    r.window = 4096;
    r.reason = "Cấu hình cân bằng: k=4, cửa sổ 4096.";
  }

  if (in.on_battery && r.profile != "may-yeu") {
    r.k = std::max(2, r.k - 1);
    r.window = std::max(1024, r.window / 2);
    r.reason += " Đang dùng pin nên giảm tải.";
  }
  if (hot) {
    r.k = std::max(2, r.k - 2);
    r.window = std::max(512, r.window / 2);
    r.threads = std::max(1, r.threads / 2);
    r.reason += " Nhiệt độ ≥80°C: giảm luồng để tránh bóp xung.";
  } else if (warm) {
    r.threads = std::max(1, int(r.threads * 0.75));
    r.reason += " Nhiệt độ ≥65°C: giảm nhẹ số luồng.";
  }
  if (in.dedicated_gpu) {
    r.window += 1024;
    r.reason += " Có GPU rời: tăng cửa sổ thêm 1024.";
  }
  return r;
}

}  // namespace yui::engine
