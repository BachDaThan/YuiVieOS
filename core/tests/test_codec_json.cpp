// Round-trip and edge-case tests for YUI-LZ / YUIP container + the JSON reader.
#include <cstring>
#include <random>

#include "testutil.hpp"
#include "yui/codec.hpp"
#include "yui/json.hpp"

using namespace yui;

static std::string repeat(const std::string& s, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) out += s;
  return out;
}

YUI_TEST(codec, empty_and_tiny) {
  codec::Bytes out;
  CHECK(codec::compress(nullptr, 0, out));
  CHECK_EQ(out.size(), size_t(1));
  codec::Bytes back;
  CHECK(codec::decompress(out, 0, back));
  CHECK_EQ(back.size(), size_t(0));

  for (size_t n : {1, 2, 3, 4, 5, 7}) {
    const std::string s(n, 'a');
    const codec::Bytes enc = codec::compress(codec::Bytes(s.begin(), s.end()));
    codec::Bytes dec;
    CHECK(codec::decompress(enc, n, dec));
    CHECK(std::string(dec.begin(), dec.end()) == s);
  }
}

YUI_TEST(codec, roundtrip_realistic) {
  // A mix of prose (compresses well) and pseudo-random bytes (incompressible).
  std::string text;
  for (int i = 0; i < 400; ++i)
    text += "YuiVieOS là hệ điều hành Việt Nam, tối ưu cho máy yếu và máy mạnh. ";
  const auto enc = codec::compress(codec::Bytes(text.begin(), text.end()));
  codec::Bytes dec;
  CHECK(codec::decompress(enc, text.size(), dec));
  CHECK(std::string(dec.begin(), dec.end()) == text);
  CHECK_MSG(enc.size() < text.size() / 4, "repetitive Vietnamese text should compress at least 4x");

  std::mt19937 rng(1234);
  codec::Bytes noise(20000);
  for (auto& b : noise) b = uint8_t(rng());
  const auto enc2 = codec::compress(noise, 6);
  codec::Bytes dec2;
  CHECK(codec::decompress(enc2, noise.size(), dec2));
  CHECK(dec2 == noise);
  CHECK_MSG(enc2.size() <= codec::bound(noise.size()), "encoded size must respect bound()");
}

YUI_TEST(codec, long_literal_and_long_match_extensions) {
  // >15 literal bytes then a very long match exercises both nibble-15 paths.
  std::string s = "0123456789abcdefghij";          // 20 unique bytes -> long literal run
  s += std::string(5000, 'Z');                     // long match of one byte repeats
  s += "tail";
  const auto enc = codec::compress(codec::Bytes(s.begin(), s.end()));
  codec::Bytes dec;
  CHECK(codec::decompress(enc, s.size(), dec));
  CHECK(std::string(dec.begin(), dec.end()) == s);
}

YUI_TEST(codec, overlapping_match_copy) {
  // Run-length data relies on the decoder's byte-by-byte overlapping copy.
  std::string s(10000, 'q');
  const auto enc = codec::compress(codec::Bytes(s.begin(), s.end()));
  CHECK_MSG(enc.size() < 64, "10k identical bytes should encode in a few dozen bytes");
  codec::Bytes dec;
  CHECK(codec::decompress(enc, s.size(), dec));
  CHECK(std::string(dec.begin(), dec.end()) == s);
}

YUI_TEST(codec, corrupt_streams_are_rejected) {
  const std::string s = repeat("yui ", 100);
  auto enc = codec::compress(codec::Bytes(s.begin(), s.end()));
  codec::Bytes dec;
  // Truncated stream.
  codec::Bytes cut(enc.begin(), enc.end() - 3);
  CHECK(!codec::decompress(cut, s.size(), dec));
  // Bad distance (larger than the bytes produced so far).
  codec::Bytes bad = enc;
  bool patched = false;
  for (size_t i = 1; i + 2 < bad.size(); ++i) {
    if (bad[i] >> 4) continue;  // find a sequence with no literals
    bad[i + 1] = 0xff;
    bad[i + 2] = 0xff;
    patched = true;
    break;
  }
  CHECK(patched);
  CHECK(!codec::decompress(bad, s.size(), dec));
}

YUI_TEST(codec, container_roundtrip_and_integrity) {
  const std::string s = repeat("Trình gọt tỉa ứng dụng — loại bỏ mã chết và nén Zstd. ", 60);
  const codec::Bytes in(s.begin(), s.end());
  const codec::Bytes packed = codec::pack(in, 7);
  CHECK(packed.size() > codec::kPackHeaderSize);
  CHECK(packed[0] == 'Y' && packed[3] == 'P');

  codec::PackInfo info;
  std::string err;
  codec::Bytes out;
  CHECK(codec::unpack(packed, out, &info, &err));
  CHECK(out == in);
  CHECK_EQ(info.original_size, in.size());
  CHECK_EQ(int(info.version), int(codec::kPackVersion));
  CHECK_EQ(info.digest_hex.size(), size_t(64));

  // Flip one payload byte: the CRC32 gate must reject it.
  codec::Bytes tampered = packed;
  tampered.back() ^= 0x01;
  CHECK(!codec::unpack(tampered, out, nullptr, &err));
  CHECK_MSG(err.find("CRC32") != std::string::npos || err.find("corrupt") != std::string::npos,
            "expected CRC/corruption error, got: " + err);
  // A mutation in the middle of the payload must also be caught.
  codec::Bytes tampered2 = packed;
  tampered2[codec::kPackHeaderSize + 10] ^= 0x80;
  CHECK(!codec::unpack(tampered2, out, nullptr, &err));
  // A tampered digest field (not covered by CRC) is caught by the SHA-256 check.
  codec::Bytes tampered3 = packed;
  tampered3[24] ^= 0xff;
  CHECK(!codec::unpack(tampered3, out, nullptr, &err));

  // Bad magic.
  codec::Bytes bad_magic = packed;
  bad_magic[0] = 'X';
  CHECK(!codec::unpack(bad_magic, out, nullptr, &err));

  // Truncated container.
  codec::Bytes truncated(packed.begin(), packed.end() - 1);
  CHECK(!codec::unpack(truncated, out, nullptr, &err));
}

YUI_TEST(codec, incompressible_input_is_stored) {
  std::mt19937 rng(7);
  codec::Bytes noise(4096);
  for (auto& b : noise) b = uint8_t(rng());
  const codec::Bytes packed = codec::pack(noise, 9);
  codec::PackInfo info;
  codec::Bytes out;
  CHECK(codec::unpack(packed, out, &info, nullptr));
  CHECK(out == noise);
  CHECK_EQ(int(info.codec), int(codec::kCodecStored));
  CHECK_MSG(packed.size() <= noise.size() + codec::kPackHeaderSize, "store mode must not inflate");
}

YUI_TEST(codec, ratio_helper) {
  CHECK(std::abs(codec::ratio(100, 25) - 0.75) < 1e-12);
  CHECK_EQ(codec::ratio(0, 0), 0.0);
}

// ------------------------------------------------------------------- JSON --
YUI_TEST(json, parse_scalars_and_containers) {
  auto r = json::parse(R"({"a":1,"b":-2,"c":1.5,"d":true,"e":null,"f":"x"})");
  CHECK(r.ok);
  CHECK_EQ(r.value["a"].as_int(), int64_t(1));
  CHECK_EQ(r.value["b"].as_int(), int64_t(-2));
  CHECK(std::abs(r.value["c"].as_double() - 1.5) < 1e-12);
  CHECK(r.value["d"].as_bool());
  CHECK(r.value["e"].is_null());
  CHECK_EQ(r.value["f"].as_string(), std::string("x"));

  auto arr = json::parse("[1,2,[3,4],{\"k\":\"v\"}]");
  CHECK(arr.ok);
  CHECK_EQ(arr.value.size(), size_t(4));
  CHECK_EQ(arr.value.arr()[2].arr()[1].as_int(), int64_t(4));

  // Insertion order must survive a round-trip.
  json::Value o;
  o.set("z", 1);
  o.set("a", 2);
  CHECK_EQ(o.dump(), std::string(R"({"z":1,"a":2})"));
}

YUI_TEST(json, unicode_and_escapes) {
  auto r = json::parse(R"({"s":"YuiVieOS \u2014 Ti\u1ebfng Vi\u1ec7t \ud83c\udf3f\n"})");
  CHECK(r.ok);
  const std::string s = r.value["s"].as_string();
  CHECK(s.find("—") != std::string::npos);          // em dash
  CHECK(s.find("Tiếng Việt") != std::string::npos);  // Vietnamese, fully decoded
  CHECK(s.find("\xF0\x9F\x8C\xBF") != std::string::npos);  // 🌿 (U+1F33F) surrogate pair
  CHECK(s.back() == '\n');

  // Serialising must re-escape the same characters and parse back identically.
  json::Value v;
  v.set("s", s);
  auto again = json::parse(v.dump());
  CHECK(again.ok);
  CHECK_EQ(again.value["s"].as_string(), s);
}

YUI_TEST(json, rejects_malformed_documents) {
  const char* bad[] = {
      "",        "{",      "{\"a\"}",  "{\"a\":}",  "[1,]",     "{\"a\":1,}",
      "tru",     "\"abc",  "{\"a\":1}x", "[1 2]",   "{\"a\":01x}", "\"\x01\"",
      "[1,2",    "{\"a\":\"\\q\"}", "nul",
  };
  for (const char* b : bad) {
    auto r = json::parse(b);
    CHECK_MSG(!r.ok, std::string("should reject: ") + b);
    CHECK_MSG(!r.error.empty(), "error message must be set");
  }
}

YUI_TEST(json, dump_pretty_and_numbers) {
  json::Value v;
  v.set("name", "YuiVieOS");
  v.set("tier", 4);
  json::Value arr;
  arr.push(1);
  arr.push("two");
  v.set("list", arr);
  const std::string pretty = v.dump(true);
  CHECK(pretty.find('\n') != std::string::npos);
  auto back = json::parse(pretty);
  CHECK(back.ok);
  CHECK_EQ(back.value.dump(), v.dump());

  json::Value d(2.0);
  CHECK_EQ(d.dump(), std::string("2.0"));  // stays a JSON double, not an int
  json::Value big(int64_t(9007199254740993LL));
  CHECK_EQ(big.dump(), std::string("9007199254740993"));
}

YUI_TEST_MAIN("test_codec_json")
