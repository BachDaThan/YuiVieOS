// Telex/VNI engine tests, using real Vietnamese words as the oracle.
#include "testutil.hpp"
#include "yui/ime.hpp"
#include "yui/text.hpp"

using namespace yui;

static void check_telex(const std::string& keys, const std::string& expected) {
  const std::string got = ime::telex(keys);
  CHECK_MSG(got == expected, "telex(\"" + keys + "\") = \"" + got + "\", expected \"" + expected + "\"");
}
static void check_vni(const std::string& keys, const std::string& expected) {
  const std::string got = ime::vni(keys);
  CHECK_MSG(got == expected, "vni(\"" + keys + "\") = \"" + got + "\", expected \"" + expected + "\"");
}

YUI_TEST(ime, telex_diacritics) {
  check_telex("aa", "â");
  check_telex("aw", "ă");
  check_telex("ee", "ê");
  check_telex("oo", "ô");
  check_telex("ow", "ơ");
  check_telex("uw", "ư");
  check_telex("dd", "đ");
  check_telex("DD", "Đ");
  check_telex("DDawng", "Đăng");
}

YUI_TEST(ime, telex_tones) {
  check_telex("a", "a");
  check_telex("as", "á");
  check_telex("af", "à");
  check_telex("ar", "ả");
  check_telex("ax", "ã");
  check_telex("aj", "ạ");
  check_telex("asz", "a");  // z removes the tone
  check_telex("aws", "ắ");
  check_telex("ees", "ế");
  check_telex("owj", "ợ");
}

YUI_TEST(ime, telex_real_words) {
  check_telex("chaof", "chào");
  check_telex("banj", "bạn");
  // Repeated onset/coda consonants are not special; the tone key is explicit.
  check_telex("cac", "cac");
  check_telex("cacs", "các");
  check_telex("tats", "tát");
  check_telex("nats", "nát");
  check_telex("mawms", "mắm");
  check_telex("cawps", "cắp");
  check_telex("toans", "toán");
  check_telex("hoas", "hóa");
  check_telex("hoaf", "hòa");  // modern placement on the first vowel of an open cluster
  check_telex("khong", "khong");  // no tone key: unchanged ASCII
  check_telex("khongs", "khóng");
  check_telex("nghieeng", "nghiêng");
  check_telex("nguyeenx", "nguyễn");
  check_telex("quaf", "quà");
  check_telex("giaf", "già");
  check_telex("tuooir", "tuổi");
  check_telex("tuwowis", "tưới");
  check_telex("khuyur", "khuỷu");
  check_telex("chuyeenj", "chuyện");
  check_telex("Tieengs Vieetj", "Tiếng Việt");
}

YUI_TEST(ime, vni_basics) {
  check_vni("a6", "â");
  check_vni("a8", "ă");
  check_vni("e6", "ê");
  check_vni("o6", "ô");
  check_vni("o7", "ơ");
  check_vni("u7", "ư");
  check_vni("d9", "đ");
  check_vni("a1", "á");
  check_vni("a2", "à");
  check_vni("a3", "ả");
  check_vni("a4", "ã");
  check_vni("a5", "ạ");
  check_vni("Tie61ng Vie65t", "Tiếng Việt");
  check_vni("cac1", "các");
  check_vni("cha2o", "chào");
  check_vni("hoa1", "hóa");
  check_vni("d9a8ng", "đăng");
  check_vni("nguye64n", "nguyễn");
}

YUI_TEST(ime, tone_placement_rules) {
  // open two-vowel cluster -> first vowel
  CHECK_EQ(ime::WordEngine::tone_position("hoa"), 1);
  // closed syllable -> last vowel
  CHECK_EQ(ime::WordEngine::tone_position("toan"), 2);
  // three-vowel cluster -> middle
  CHECK_EQ(ime::WordEngine::tone_position("khoai"), 3);
  CHECK_EQ(ime::WordEngine::tone_position("tuoi"), 2);
  // "qu" glide is skipped
  CHECK_EQ(ime::WordEngine::tone_position("qua"), 2);
  CHECK_EQ(ime::WordEngine::tone_position("gia"), 2);
  // no vowel at all
  CHECK_EQ(ime::WordEngine::tone_position("xyz"), 1);  // y is a vowel in Vietnamese
  CHECK_EQ(ime::WordEngine::tone_position("bcdf"), -1);
}

YUI_TEST(ime, passthrough_and_boundaries) {
  // Already-composed Vietnamese must survive untouched.
  CHECK_EQ(ime::telex("Xin chào thế giới"), std::string("Xin chào thế giới"));
  // Punctuation is preserved and ends the word.
  CHECK_EQ(ime::telex("chaof, banj!"), std::string("chào, bạn!"));
  // Numbers typed with VNI outside a word stay literal.
  CHECK_EQ(ime::vni("2026"), std::string("2026"));
  // Uppercase tone keys still work: S is sắc and only acts once a vowel exists.
  CHECK_EQ(ime::telex("OS"), std::string("Ó"));
  CHECK_EQ(ime::telex("YuiVie"), std::string("YuiVie"));
}

YUI_TEST(ime, english_auto_restore) {
  for (const std::string word : {"word", "window", "software", "user", "linux"}) {
    ime::Composition comp;
    std::string typed;
    for (char c : word) {
      typed.push_back(c);
      CHECK(comp.feed(static_cast<uint32_t>(c)));
      CHECK_EQ(comp.preedit(), typed);
    }
    CHECK_EQ(comp.commit(), word);
  }

  for (const std::string word : {"WORD", "Software", "Linux"}) {
    ime::Composition comp;
    for (char c : word) CHECK(comp.feed(static_cast<uint32_t>(c)));
    CHECK_EQ(comp.commit(), word);
  }

  // The same safeguard applies in VNI mode, whose letter transforms overlap Telex.
  for (const std::string word : {"word", "window", "software", "user", "linux"}) {
    ime::Composition comp(ime::Method::Vni);
    for (char c : word) CHECK(comp.feed(static_cast<uint32_t>(c)));
    CHECK_EQ(comp.preedit(), word);
    CHECK_EQ(comp.commit(), word);
  }
}

YUI_TEST(ime, word_engine_incremental) {
  ime::WordEngine eng(ime::Method::Telex);
  CHECK_EQ(eng.feed('v'), std::string("v"));
  CHECK_EQ(eng.feed('i'), std::string("vi"));
  CHECK_EQ(eng.feed('e'), std::string("vie"));
  CHECK_EQ(eng.feed('e'), std::string("viê"));
  CHECK_EQ(eng.feed('t'), std::string("viêt"));
  CHECK_EQ(eng.feed('j'), std::string("việt"));
  CHECK_EQ(eng.feed(' '), std::string(""));  // boundary resets the word
  CHECK(eng.empty());
  CHECK_EQ(eng.feed('d'), std::string("d"));
  CHECK_EQ(eng.feed('d'), std::string("đ"));

  eng.set_method(ime::Method::Vni);
  CHECK_EQ(eng.feed('a'), std::string("a"));
  CHECK_EQ(eng.feed('6'), std::string("â"));
  CHECK_EQ(eng.feed('1'), std::string("ấ"));
}

YUI_TEST(ime, editable_composition_buffer) {
  ime::Composition comp;
  CHECK(comp.empty());
  CHECK(comp.feed('T'));
  CHECK(comp.feed('i'));
  CHECK(comp.feed('e'));
  CHECK(comp.feed('e'));
  CHECK(comp.feed('n'));
  CHECK(comp.feed('g'));
  CHECK(comp.feed('s'));
  CHECK_EQ(comp.preedit(), std::string("Tiếng"));
  CHECK(comp.backspace());
  CHECK_EQ(comp.preedit(), std::string("Tiêng"));
  CHECK(comp.feed('s'));
  CHECK_EQ(comp.commit(), std::string("Tiếng"));
  CHECK(comp.empty());
  CHECK(!comp.backspace());
  CHECK(!comp.feed(' '));
  CHECK(!comp.feed(0x1f600));

  comp.set_method(ime::Method::Vni);
  CHECK(comp.feed('a'));
  CHECK(comp.feed('6'));
  CHECK(comp.feed('1'));
  CHECK_EQ(comp.preedit(), std::string("ấ"));
  CHECK(comp.backspace());
  CHECK_EQ(comp.preedit(), std::string("â"));
  CHECK_EQ(comp.commit(), std::string("â"));

  comp.set_method(ime::Method::Telex);
  for (char c : std::string("window")) CHECK(comp.feed(static_cast<uint32_t>(c)));
  CHECK_EQ(comp.preedit(), std::string("window"));
  CHECK_EQ(comp.commit(), std::string("window"));

  for (char c : std::string("wordx")) CHECK(comp.feed(static_cast<uint32_t>(c)));
  CHECK_EQ(comp.restore(), std::string("wordx"));  // Escape-style literal recovery
  CHECK(comp.empty());
}

YUI_TEST(ime, display_width_of_vietnamese) {
  // Pre-composed Vietnamese characters are single code points, width 1 each.
  CHECK_EQ(text::length("Tiếng Việt"), size_t(10));
  CHECK_EQ(text::display_width("Tiếng Việt"), size_t(10));
  CHECK_EQ(text::fold("TIẾNG VIỆT"), std::string("tiếng việt"));
  CHECK(text::contains_ci("Tiếng Việt", "việt"));
  CHECK(text::contains_ci("Đường", "đường"));
  CHECK(!text::contains_ci("Tiếng Việt", "tây"));
}

YUI_TEST_MAIN("test_ime")
