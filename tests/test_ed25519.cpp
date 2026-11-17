// Vector kiểm thử chuẩn cho Ed25519 + SHA-512.
//
// Nguồn vector (bắt buộc, không tự sinh):
//   * SHA-512 : FIPS 180-4 ("" , "abc", chuỗi 112 ký tự "abcdefgh...")
//   * Ed25519 : RFC 8032 §7.1 — TEST 1, TEST 2, TEST 3, TEST 1024, TEST SHA(abc)
// Chữ ký Ed25519 là **xác định**: sign(seed, msg) phải tạo ra đúng byte của
// RFC, không chỉ "verify được" — nên vector so cả chữ ký thô.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "testutil.hpp"
#include "yui/bundle.hpp"
#include "yui/crypto.hpp"
#include "yui/ed25519.hpp"

using namespace yui;
using namespace yui::ed25519;

namespace {

std::vector<uint8_t> unhex(const std::string& hex) {
  std::vector<uint8_t> out(hex.size() / 2);
  if (!from_hex(hex, out.data(), out.size())) {
    out.clear();
  }
  return out;
}

Seed seed_of(const std::string& hex) {
  Seed s{};
  from_hex(hex, s.data(), s.size());
  return s;
}

}  // namespace

// ------------------------------------------------------------------ SHA-512 --
YUI_TEST(ed25519, sha512_fips_vectors) {
  // FIPS 180-4: SHA-512("")
  const auto d0 = sha512("", 0);
  CHECK_EQ(to_hex(d0.bytes.data(), 64),
           std::string("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                       "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
  // FIPS 180-4: SHA-512("abc")
  const auto d1 = sha512("abc", 3);
  CHECK_EQ(to_hex(d1.bytes.data(), 64),
           std::string("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                       "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
  // FIPS 180-4: SHA-512("abcdefgh…tuv") — 112 ký tự, ép hai khối.
  const std::string long_msg = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                               "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  const auto d2 = sha512(long_msg.data(), long_msg.size());
  CHECK_EQ(to_hex(d2.bytes.data(), 64),
           std::string("8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                       "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));
}

YUI_TEST(ed25519, sha512_streaming_matches_oneshot) {
  const std::string msg = "YuiVieOS — kiểm thử SHA-512 theo từng mảnh1234567890";
  const auto one = sha512(msg.data(), msg.size());
  Sha512 h;
  for (size_t i = 0; i < msg.size(); i += 7) {
    h.update(msg.data() + i, std::min<size_t>(7, msg.size() - i));
  }
  const auto streamed = h.finalize();
  CHECK(streamed.bytes == one.bytes);
  // finalize không phá trạng thái: gọi lần nữa vẫn ra đúng digest.
  CHECK(h.finalize().bytes == one.bytes);
}

// ----------------------------------------------------------------- Ed25519 --
YUI_TEST(ed25519, rfc8032_test1_empty_message) {
  const Seed seed = seed_of("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  const PublicKey pub = public_key(seed);
  CHECK_EQ(to_hex(pub.data(), 32),
           std::string("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
  const Signature sig = sign(seed, nullptr, 0);
  CHECK_EQ(to_hex(sig.data(), 64),
           std::string("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                       "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"));
  CHECK(verify(pub, nullptr, 0, sig));
}

YUI_TEST(ed25519, rfc8032_test2_one_byte) {
  const Seed seed = seed_of("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
  const PublicKey pub = public_key(seed);
  CHECK_EQ(to_hex(pub.data(), 32),
           std::string("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"));
  const uint8_t msg[1] = {0x72};
  const Signature sig = sign(seed, msg, 1);
  CHECK_EQ(to_hex(sig.data(), 64),
           std::string("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
                       "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"));
  CHECK(verify(pub, msg, 1, sig));
}

YUI_TEST(ed25519, rfc8032_test3_two_bytes) {
  const Seed seed = seed_of("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7");
  const PublicKey pub = public_key(seed);
  CHECK_EQ(to_hex(pub.data(), 32),
           std::string("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025"));
  const auto msg = unhex("af82");
  CHECK_EQ(msg.size(), size_t(2));
  const Signature sig = sign(seed, msg);
  CHECK_EQ(to_hex(sig.data(), 64),
           std::string("6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
                       "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"));
  CHECK(verify(pub, msg, sig));
}

YUI_TEST(ed25519, rfc8032_test_sha_abc) {
  // Message = SHA-512("abc") — 64 byte, ép đường đệm của hash nội bộ.
  const Seed seed = seed_of("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42");
  const PublicKey pub = public_key(seed);
  CHECK_EQ(to_hex(pub.data(), 32),
           std::string("ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf"));
  const auto msg = unhex("ddaf35a193617abacc417349ae204131"
                         "12e6fa4e89a97ea20a9eeee64b55d39a"
                         "2192992a274fc1a836ba3c23a3feebbd"
                         "454d4423643ce80e2a9ac94fa54ca49f");
  CHECK_EQ(msg.size(), size_t(64));
  const Signature sig = sign(seed, msg);
  CHECK_EQ(to_hex(sig.data(), 64),
           std::string("dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
                       "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"));
  CHECK(verify(pub, msg, sig));
}

YUI_TEST(ed25519, rfc8032_test1024_long_message) {
  const Seed seed = seed_of("f5e5767cf153319517630f226876b86c8160cc583bc013744c6bf255f5cc0ee5");
  const PublicKey pub = public_key(seed);
  CHECK_EQ(to_hex(pub.data(), 32),
           std::string("278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e"));
  const auto msg = unhex(
      "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98"
      "fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
      "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d"
      "658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
      "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4fe"
      "ba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
      "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbef"
      "efd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
      "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed1"
      "85ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
      "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24"
      "554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
      "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc"
      "2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
      "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128ba"
      "b27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
      "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429e"
      "c96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
      "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c"
      "42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
      "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34df"
      "f7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
      "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649"
      "de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
      "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a3"
      "2ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
      "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5f"
      "b93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
      "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1"
      "369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
      "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c"
      "0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0");
  CHECK_EQ(msg.size(), size_t(1023));
  const Signature sig = sign(seed, msg);
  CHECK_EQ(to_hex(sig.data(), 64),
           std::string("0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350"
                       "aa5371b1508f9f4528ecea23c436d94b5e8fcd4f681e30a6ac00a9704a188a03"));
  CHECK(verify(pub, msg, sig));
}

YUI_TEST(ed25519, tamper_and_malleability_rejected) {
  const Seed seed = seed_of("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  const PublicKey pub = public_key(seed);
  const std::string msg = "bundle manifest v1";
  const Signature sig = sign(seed, msg.data(), msg.size());
  CHECK(verify(pub, msg.data(), msg.size(), sig));

  // Sửa 1 byte thông điệp ⇒ từ chối.
  std::string tampered = msg;
  tampered[0] ^= 1;
  CHECK(!verify(pub, tampered.data(), tampered.size(), sig));

  // Sửa 1 byte chữ ký ⇒ từ chối.
  Signature bad = sig;
  bad[10] ^= 0x80;
  CHECK(!verify(pub, msg.data(), msg.size(), bad));

  // Khóa công khai khác ⇒ từ chối.
  const Seed other = seed_of("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
  CHECK(!verify(public_key(other), msg.data(), msg.size(), sig));

  // S >= L ⇒ từ chối (RFC 8032 §5.1.7: S ngoài miền là chữ ký vô hiệu).
  Signature huge = sig;
  for (int i = 32; i < 64; ++i) huge[i] = 0xff;
  CHECK(!verify(pub, msg.data(), msg.size(), huge));

  // Khóa rác không nạp được điểm ⇒ từ chối êm (không treo, không crash).
  PublicKey junk{};
  junk.fill(0xff);
  junk[31] = 0x7f;
  CHECK(!verify(junk, msg.data(), msg.size(), sig));
}

// ------------------------------------------------------ Sovereign Keyring --
namespace {

std::string make_tmpdir() {
  char tmpl[] = "/tmp/yui-bundle-test-XXXXXX";
  char* d = ::mkdtemp(tmpl);
  return d ? std::string(d) : std::string();
}

void mkdir_p_local(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty()) {
        ::mkdir(cur.c_str(), 0755);
      }
      if (i < path.size()) cur += '/';
    } else {
      cur += path[i];
    }
  }
}

void write_raw(const std::string& path, const std::string& content) {
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos) {
    mkdir_p_local(path.substr(0, slash));
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f) {
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
  }
}

bool read_raw(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[1024];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

std::string hex_sha(const std::string& content) {
  return ed25519::to_hex(crypto::sha256(content).data(), 32);
}

// Dựng một bundle thật trong thư mục tạm: payload + manifest + chữ ký Ed25519.
std::string build_bundle(const std::string& dir, const ed25519::Seed& seed,
                         const std::string& key_id, const std::string& pretty_manifest) {
  (void)pretty_manifest;
  system(("mkdir -p " + dir + "/payload/bin").c_str());
  write_raw(dir + "/payload/bin/yuid", "#!/bin/sh\necho yui\n");
  write_raw(dir + "/payload/README.md", "YuiVieOS bundle\n");

  bundle::Manifest m;
  m.version = "1.0.0";
  m.key_id = key_id;
  m.files.push_back({"bin/yuid", hex_sha("#!/bin/sh\necho yui\n"), "0755"});
  m.files.push_back({"README.md", hex_sha("YuiVieOS bundle\n"), "0644"});

  // manifest.json cố ý viết KHÁC định dạng chuỗi chuẩn (thứ tự khoá, khoảng
  // trắng) — verifier phải dựng lại chuỗi chuẩn từ trường đã parse, không
  // được tin byte nhận được.
  const std::string manifest_json =
      std::string("{\n  \"version\": \"") + m.version + "\",\n  \"key_id\": \"" + key_id +
      "\",\n  \"files\": [\n"
      "    {\"sha256\": \"" + hex_sha("YuiVieOS bundle\n") + "\", \"path\": \"README.md\", \"mode\": \"0644\"},\n"
      "    {\"mode\": \"0755\", \"path\": \"bin/yuid\", \"sha256\": \"" + hex_sha("#!/bin/sh\necho yui\n") + "\"}\n"
      "  ]\n}\n";
  write_raw(dir + "/manifest.json", manifest_json);

  const std::string canon = bundle::canonical(m);
  const ed25519::Signature sig = ed25519::sign(seed, canon.data(), canon.size());
  write_raw(dir + "/manifest.sig", std::string(reinterpret_cast<const char*>(sig.data()), 64));
  return dir;
}

}  // namespace

YUI_TEST(bundle, keyring_crud_and_persistence) {
  bundle::Keyring keys;
  ed25519::PublicKey k1{}, k2{};
  k1.fill(1);
  k2.fill(2);
  CHECK(keys.add("release-2026", k1));
  CHECK(!keys.add("release-2026", k2));  // không ghi đè im lặng
  CHECK(keys.add("release-2027", k2));
  CHECK_EQ(keys.size(), size_t(2));
  CHECK(keys.contains("release-2026"));
  CHECK(!keys.contains("nope"));
  ed25519::PublicKey out{};
  CHECK(keys.get("release-2027", out));
  CHECK(out == k2);
  CHECK(!keys.get("nope", out));

  // save_dir / load_dir: kho khoá đi qua USB, không qua mạng.
  const std::string dir = make_tmpdir();
  std::string err;
  CHECK_MSG(keys.save_dir(dir + "/trusted-keys", &err), err);
  bundle::Keyring loaded;
  CHECK_MSG(loaded.load_dir(dir + "/trusted-keys", &err), err);
  CHECK_EQ(loaded.size(), size_t(2));
  CHECK(loaded.get("release-2026", out));
  CHECK(out == k1);

  CHECK(keys.remove("release-2026"));
  CHECK(!keys.remove("release-2026"));
  CHECK_EQ(keys.size(), size_t(1));
}

YUI_TEST(bundle, sign_verify_offline_ok) {
  const ed25519::Seed seed = seed_of("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  bundle::Keyring keys;
  keys.add("release-2026", public_key(seed));
  const std::string dir = build_bundle(make_tmpdir(), seed, "release-2026", "");

  const auto r = bundle::verify(dir, keys);
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(r.signature_ok);
  CHECK(r.payload_ok);
  CHECK_EQ(r.key_id, std::string("release-2026"));
  CHECK(r.reason_vi.find("hợp lệ") != std::string::npos);
}

YUI_TEST(bundle, tamper_any_byte_rejected) {
  const ed25519::Seed seed = seed_of("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
  bundle::Keyring keys;
  keys.add("release-2026", public_key(seed));

  // 1) Sửa 1 byte payload ⇒ SHA-256 lệch ⇒ từ chối.
  {
    const std::string dir = build_bundle(make_tmpdir(), seed, "release-2026", "");
    write_raw(dir + "/payload/bin/yuid", "#!/bin/sh\necho YUI\n");  // sửa 1 ký tự
    const auto r = bundle::verify(dir, keys);
    CHECK(!r.ok);
    CHECK(!r.payload_ok);
    CHECK(r.reason_vi.find("SHA-256") != std::string::npos);
    CHECK(r.reason_vi.find("bin/yuid") != std::string::npos);
  }
  // 2) Sửa manifest (dù chỉ version) ⇒ chữ ký không khớp ⇒ từ chối.
  {
    const std::string dir = build_bundle(make_tmpdir(), seed, "release-2026", "");
    write_raw(dir + "/manifest.json", "{\"version\":\"9.9.9\",\"key_id\":\"release-2026\",\"files\":[]}");
    const auto r = bundle::verify(dir, keys);
    CHECK(!r.ok);
    CHECK(!r.signature_ok);
    CHECK(r.reason_vi.find("chữ ký") != std::string::npos);
  }
  // 3) Sửa 1 byte chữ ký ⇒ từ chối.
  {
    const std::string dir = build_bundle(make_tmpdir(), seed, "release-2026", "");
    FILE* f = std::fopen((dir + "/manifest.sig").c_str(), "r+b");
    CHECK(f != nullptr);
    if (f) {
      std::fseek(f, 5, SEEK_SET);
      std::fputc(0xAA, f);
      std::fclose(f);
    }
    const auto r = bundle::verify(dir, keys);
    CHECK(!r.ok);
  }
  // 4) Khoá lạ (không trong keyring) ⇒ từ chối, lý do nêu rõ khoá.
  {
    const std::string dir = build_bundle(make_tmpdir(), seed, "release-2026", "");
    bundle::Keyring empty;
    const auto r = bundle::verify(dir, empty);
    CHECK(!r.ok);
    CHECK(r.reason_vi.find("release-2026") != std::string::npos);
    CHECK(r.reason_vi.find("từ chối") != std::string::npos);
  }
  // 5) manifest chứa đường dẫn nguy hiểm (../) ⇒ parse từ chối.
  {
    const std::string dir = build_bundle(make_tmpdir(), seed, "release-2026", "");
    write_raw(dir + "/manifest.json",
              "{\"version\":\"1\",\"key_id\":\"release-2026\",\"files\":"
              "[{\"path\":\"../../etc/passwd\",\"sha256\":\"" + std::string(64, 'a') + "\",\"mode\":\"0644\"}]}");
    const auto r = bundle::verify(dir, keys);
    CHECK(!r.ok);
    CHECK(r.reason_vi.find("không an toàn") != std::string::npos);
  }
}

YUI_TEST(bundle, install_ab_slots_and_rollback) {
  const ed25519::Seed seed = seed_of("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7");
  bundle::Keyring keys;
  keys.add("release-2026", public_key(seed));
  const std::string dir = build_bundle(make_tmpdir(), seed, "release-2026", "");
  const std::string state = make_tmpdir() + "/state";

  CHECK_EQ(bundle::active_slot(state), std::string("A"));  // chưa cài ⇒ A

  const auto i1 = bundle::install(dir, keys, state);
  CHECK_MSG(i1.ok, i1.reason_vi);
  CHECK_EQ(i1.slot, std::string("B"));  // cài vào slot KHÔNG hoạt động
  CHECK_EQ(bundle::active_slot(state), std::string("B"));
  // Payload nằm trọn vẹn trong slot B.
  FILE* f = std::fopen((state + "/slots/B/bin/yuid").c_str(), "rb");
  CHECK(f != nullptr);
  if (f) std::fclose(f);

  // Cài lần hai ⇒ quay sang slot A, slot B cũ giữ nguyên làm rollback.
  const auto i2 = bundle::install(dir, keys, state);
  CHECK_MSG(i2.ok, i2.reason_vi);
  CHECK_EQ(i2.slot, std::string("A"));
  CHECK_EQ(bundle::active_slot(state), std::string("A"));
  f = std::fopen((state + "/slots/A/README.md").c_str(), "rb");
  CHECK(f != nullptr);
  if (f) std::fclose(f);
  f = std::fopen((state + "/slots/B/README.md").c_str(), "rb");
  CHECK(f != nullptr);  // rollback target còn nguyên
  if (f) std::fclose(f);

  // Bundle hỏng: không cài được VÀ con trỏ boot không nhúc nhích.
  write_raw(dir + "/payload/README.md", "SUA ROI\n");
  const auto i3 = bundle::install(dir, keys, state);
  CHECK(!i3.ok);
  CHECK(i3.reason_vi.find("không cài được") != std::string::npos);
  CHECK_EQ(bundle::active_slot(state), std::string("A"));
}

YUI_TEST(bundle, sign_bundle_and_verify_offline) {
  const ed25519::Seed seed = seed_of("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  const auto pub = ed25519::public_key(seed);

  bundle::Keyring keys;
  CHECK(keys.add("vietnam-release-2026", pub));

  const std::string payload_dir = make_tmpdir() + "/src_payload";
  write_raw(payload_dir + "/bin/yuid", "#!/bin/sh\necho yuid\n");
  write_raw(payload_dir + "/etc/config.json", "{\"version\":\"2.0\"}\n");
  write_raw(payload_dir + "/share/doc/README", "Sovereign release\n");

  const std::string bundle_dir = make_tmpdir() + "/built_bundle";
  bundle::SignOptions opts;
  opts.key_id = "vietnam-release-2026";
  opts.version = "2.1.0";
  opts.stream = "os";

  std::string err;
  const bool ok = bundle::sign_bundle(payload_dir, bundle_dir, opts, seed, &err);
  CHECK_MSG(ok, err);

  // Xác minh offline bằng Keyring
  const auto r = bundle::verify(bundle_dir, keys);
  CHECK(r.ok);
  CHECK(r.signature_ok);
  CHECK(r.payload_ok);
  CHECK_EQ(r.key_id, std::string("vietnam-release-2026"));
  CHECK(r.reason_vi.find("hợp lệ") != std::string::npos);

  // Thử phá hoại payload 1 byte -> verify từ chối
  write_raw(bundle_dir + "/payload/share/doc/README", "Sovereign tampered\n");
  const auto r_tamper = bundle::verify(bundle_dir, keys);
  CHECK(!r_tamper.ok);
  CHECK(!r_tamper.payload_ok);
  CHECK(r_tamper.reason_vi.find("SHA-256 không khớp") != std::string::npos);

  // Khôi phục lại và thử phá hoại chữ ký -> verify từ chối
  write_raw(bundle_dir + "/payload/share/doc/README", "Sovereign release\n");
  std::vector<uint8_t> sig_bytes;
  CHECK(read_raw(bundle_dir + "/manifest.sig", sig_bytes));
  CHECK(sig_bytes.size() >= 64);
  if (sig_bytes.size() >= 64) {
    sig_bytes[5] ^= 0x55;
    FILE* fs = std::fopen((bundle_dir + "/manifest.sig").c_str(), "wb");
    if (fs) {
      std::fwrite(sig_bytes.data(), 1, sig_bytes.size(), fs);
      std::fclose(fs);
    }
  }
  const auto r_sig_tamper = bundle::verify(bundle_dir, keys);
  CHECK(!r_sig_tamper.ok);
  CHECK(!r_sig_tamper.signature_ok);
  CHECK(r_sig_tamper.reason_vi.find("chữ ký Ed25519") != std::string::npos);
}

YUI_TEST(bundle, update_deferred_apply_and_commit) {
  const ed25519::Seed seed = seed_of("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
  bundle::Keyring keys;
  keys.add("rel-key", ed25519::public_key(seed));

  const std::string payload = make_tmpdir() + "/payload";
  write_raw(payload + "/kernel", "YuiKernel v2.0\n");

  const std::string bundle_dir = make_tmpdir() + "/bundle";
  bundle::SignOptions s_opts;
  s_opts.key_id = "rel-key";
  s_opts.version = "2.0.0";
  s_opts.stream = "os";
  CHECK(bundle::sign_bundle(payload, bundle_dir, s_opts, seed));

  const std::string state = make_tmpdir() + "/state";

  // Ban đầu: active slot là A, chưa có pending
  CHECK_EQ(bundle::stream_active_slot(state, "os"), std::string("A"));
  auto st0 = bundle::update_status(state, "os");
  CHECK_EQ(st0.active_slot, std::string("A"));
  CHECK(!st0.has_pending);

  // Áp dụng với hoãn kích hoạt (defer_reboot = true)
  bundle::UpdateOptions u_opts;
  u_opts.stream = "os";
  u_opts.defer_reboot = true;

  const auto r_apply = bundle::update_apply(bundle_dir, keys, state, u_opts);
  CHECK(r_apply.ok);
  CHECK(r_apply.pending);
  CHECK_EQ(r_apply.slot, std::string("B"));

  // Quan trọng: Phiên làm việc của người dùng KHÔNG bị gián đoạn, boot pointer vẫn là A!
  CHECK_EQ(bundle::stream_active_slot(state, "os"), std::string("A"));
  auto st1 = bundle::update_status(state, "os");
  CHECK_EQ(st1.active_slot, std::string("A"));
  CHECK(st1.has_pending);
  CHECK_EQ(st1.pending_slot, std::string("B"));
  CHECK_EQ(st1.backup_version, std::string("2.0.0"));

  // Khi khởi động máy / commit pending: chuyển sang khe B
  std::string commit_err;
  CHECK(bundle::update_commit_pending(state, "os", &commit_err));

  // Giờ khe B đã trở thành active!
  CHECK_EQ(bundle::stream_active_slot(state, "os"), std::string("B"));
  auto st2 = bundle::update_status(state, "os");
  CHECK_EQ(st2.active_slot, std::string("B"));
  CHECK(!st2.has_pending);
  CHECK_EQ(st2.active_version, std::string("2.0.0"));

  // Quay lui về khe A
  const auto r_roll = bundle::update_rollback(state, "os");
  CHECK(r_roll.ok);
  CHECK_EQ(r_roll.slot, std::string("A"));
  CHECK_EQ(bundle::stream_active_slot(state, "os"), std::string("A"));
}

YUI_TEST(bundle, multi_stream_isolation) {
  const ed25519::Seed seed = seed_of("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7");
  bundle::Keyring keys;
  keys.add("vietnam-key", ed25519::public_key(seed));

  const std::string state = make_tmpdir() + "/state";

  // Chuẩn bị bundle cho luồng models
  const std::string payload_models = make_tmpdir() + "/models_payload";
  write_raw(payload_models + "/weights.bin", "model-weights-4bit-kv\n");
  const std::string bundle_models = make_tmpdir() + "/models_bundle";
  bundle::SignOptions m_opts;
  m_opts.key_id = "vietnam-key";
  m_opts.version = "0.5.0";
  m_opts.stream = "models";
  CHECK(bundle::sign_bundle(payload_models, bundle_models, m_opts, seed));

  // Chuẩn bị bundle cho luồng apps
  const std::string payload_apps = make_tmpdir() + "/apps_payload";
  write_raw(payload_apps + "/editor.app", "text-editor-app\n");
  const std::string bundle_apps = make_tmpdir() + "/apps_bundle";
  bundle::SignOptions a_opts;
  a_opts.key_id = "vietnam-key";
  a_opts.version = "1.2.0";
  a_opts.stream = "apps";
  CHECK(bundle::sign_bundle(payload_apps, bundle_apps, a_opts, seed));

  // Áp dụng vào luồng models (khe B, kích hoạt ngay)
  bundle::UpdateOptions opts_m;
  opts_m.stream = "models";
  opts_m.defer_reboot = false;
  const auto r_m = bundle::update_apply(bundle_models, keys, state, opts_m);
  CHECK(r_m.ok);
  CHECK_EQ(r_m.slot, std::string("B"));

  // Kiểm tra tính độc lập tuyệt đối giữa các luồng:
  // Luồng models đã sang B:
  CHECK_EQ(bundle::stream_active_slot(state, "models"), std::string("B"));
  // Luồng os vẫn ở A nguyên vẹn:
  CHECK_EQ(bundle::stream_active_slot(state, "os"), std::string("A"));
  // Luồng apps vẫn ở A nguyên vẹn:
  CHECK_EQ(bundle::stream_active_slot(state, "apps"), std::string("A"));

  // Áp dụng vào luồng apps với defer_reboot = true
  bundle::UpdateOptions opts_a;
  opts_a.stream = "apps";
  opts_a.defer_reboot = true;
  const auto r_a = bundle::update_apply(bundle_apps, keys, state, opts_a);
  CHECK(r_a.ok);
  CHECK(r_a.pending);

  auto st_apps = bundle::update_status(state, "apps");
  CHECK_EQ(st_apps.active_slot, std::string("A"));
  CHECK(st_apps.has_pending);
  CHECK_EQ(st_apps.pending_slot, std::string("B"));

  // Luồng models vẫn chạy trực tiếp trên B, không có pending
  auto st_models = bundle::update_status(state, "models");
  CHECK_EQ(st_models.active_slot, std::string("B"));
  CHECK(!st_models.has_pending);
  CHECK_EQ(st_models.active_version, std::string("0.5.0"));

  // Quay lui models về A
  const auto r_m_roll = bundle::update_rollback(state, "models");
  CHECK(r_m_roll.ok);
  CHECK_EQ(bundle::stream_active_slot(state, "models"), std::string("A"));

  // os và apps hoàn toàn không bị ảnh hưởng bởi rollback của models
  CHECK_EQ(bundle::stream_active_slot(state, "os"), std::string("A"));
  CHECK(bundle::update_status(state, "apps").has_pending);
}

YUI_TEST_MAIN("ed25519")
