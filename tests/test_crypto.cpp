// Test vectors for the YuiVault crypto primitives.
// Every expected value was produced by an independent implementation
// (Python hashlib/hmac) and is documented inline so the tests stay auditable.
#include "testutil.hpp"

#include <algorithm>

#include "yui/crypto.hpp"

using namespace yui::crypto;

YUI_TEST(crypto, sha256_vectors) {
  CHECK_HEX(hex(sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_HEX(hex(sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_HEX(hex(sha256("The quick brown fox jumps over the lazy dog")),
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

YUI_TEST(crypto, sha256_streaming_matches_oneshot) {
  // Feeding a long message in awkward chunk sizes must give the same digest.
  std::string msg;
  for (int i = 0; i < 1000; ++i) msg += char('a' + (i % 26));
  const Digest32 one = sha256(msg);
  Sha256 s;
  for (size_t off = 0; off < msg.size();) {
    const size_t take = std::min(std::min<size_t>(1, msg.size() - off), 7 + (off % 13));
    s.update(msg.data() + off, take);
    off += take;
  }
  CHECK(s.final() == one);
  // Repeated final() must not leak state between runs.
  Sha256 t;
  t.update("abc");
  CHECK(t.final() == sha256("abc"));
  CHECK(t.final() == sha256(""));
}

YUI_TEST(crypto, hmac_sha256_vectors) {
  const std::string fox = "The quick brown fox jumps over the lazy dog";
  CHECK_HEX(hex(hmac_sha256(Bytes{'k', 'e', 'y'}, Bytes(fox.begin(), fox.end()))),
            "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
  // RFC 4231 test case 1.
  Bytes key(20, 0x0b);
  Bytes msg{'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e'};
  CHECK_HEX(hex(hmac_sha256(key, msg)), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  // RFC 4231 test case 6: key longer than the block size.
  Bytes longkey(131, 0xaa);
  const std::string rfc6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  CHECK_HEX(hex(hmac_sha256(longkey, Bytes(rfc6.begin(), rfc6.end()))),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

YUI_TEST(crypto, ct_equal) {
  const uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 4}, c[4] = {1, 2, 3, 5};
  CHECK(ct_equal(a, b, 4));
  CHECK(!ct_equal(a, c, 4));
}

YUI_TEST(crypto, shake256_vectors) {
  CHECK_HEX(hex(shake256("", 32)), "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f");
  CHECK_HEX(hex(shake256("abc", 32)), "483366601360a8771c6863080cc4114d8db44530f8f1e1ee4f94ea37e78b5739");
  // Squeezing in small chunks must equal one big squeeze (long output, >1 rate).
  const Bytes big = shake256("YuiVieOS", 500);
  Shake256 s;
  s.absorb("YuiVieOS", 8);
  Bytes chunked(500);
  size_t off = 0;
  const size_t sizes[] = {1, 2, 3, 55, 100, 339};
  for (size_t n : sizes) {
    s.squeeze(chunked.data() + off, n);
    off += n;
  }
  CHECK(big == chunked);
  CHECK(big.size() == 500);
}

YUI_TEST(crypto, shake256_absorb_blocks) {
  // Absorbing data longer than the 136-byte rate must match the one-shot value.
  std::string msg(1000, 'x');
  const Bytes a = shake256(msg, 64);
  Shake256 s;
  for (size_t off = 0; off < msg.size(); off += 137) s.absorb(msg.data() + off, std::min<size_t>(137, msg.size() - off));
  Bytes b(64);
  s.squeeze(b.data(), 64);
  CHECK(a == b);
}

YUI_TEST(crypto, scrypt_rfc7914_vectors) {
  Bytes out;
  // RFC 7914 §12: scrypt("", "", 16, 1, 1, 64)
  CHECK(scrypt("", "", 16, 1, 1, out, 64));
  CHECK_HEX(hex(out),
            "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442"
            "fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906");
  // RFC 7914 §12: scrypt("password", "NaCl", 1024, 8, 16, 64)
  CHECK(scrypt("password", "NaCl", 1024, 8, 16, out, 64));
  CHECK_HEX(hex(out),
            "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162"
            "2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640");
}

YUI_TEST(crypto, scrypt_rejects_bad_params) {
  Bytes out;
  CHECK(!scrypt("pw", "salt", 15, 8, 1, out, 32));     // not a power of two
  CHECK(!scrypt("pw", "salt", 1, 8, 1, out, 32));      // N too small
  CHECK(!scrypt("pw", "salt", 1024, 0, 1, out, 32));   // r must be > 0
  CHECK(!scrypt("pw", "salt", 1024, 8, 0, out, 32));   // p must be > 0
}

YUI_TEST(crypto, helpers_roundtrip) {
  const Bytes r = random_bytes(32);
  CHECK(r.size() == 32);
  const Bytes r2 = random_bytes(32);
  CHECK(r != r2);  // 2^-256 chance of a false positive
  CHECK(from_hex(hex(r)) == r);
  CHECK(from_hex("de:ad:be:ef") == (Bytes{0xde, 0xad, 0xbe, 0xef}));
}

YUI_TEST_MAIN("test_crypto")
