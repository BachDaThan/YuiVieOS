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

static void apply_replay(std::string& visible, const ime::ReplayEdit& edit) {
  for (size_t i = 0; i < edit.backspaces; ++i) {
    size_t pos = visible.size();
    while (pos > 0 && (static_cast<unsigned char>(visible[pos - 1]) & 0xc0) == 0x80) --pos;
    if (pos > 0) visible.resize(pos - 1);
  }
  visible += edit.text;
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

YUI_TEST(ime, direct_mode_exact_telex_replay) {
  ime::DirectComposition direct;
  std::string visible;
  size_t total_backspaces = 0;
  auto feed = [&](uint32_t key, size_t backspaces, const std::string& committed,
                  const std::string& expected_visible) {
    ime::ReplayEdit edit;
    CHECK(direct.feed(key, &edit));
    CHECK_EQ(edit.backspaces, backspaces);
    CHECK_EQ(edit.text, committed);
    total_backspaces += edit.backspaces;
    apply_replay(visible, edit);
    CHECK_EQ(visible, expected_visible);
  };

  feed('d', 0, "d", "d");
  feed('d', 1, "đ", "đ");  // dd replaces the prior d with one Backspace, then đ
  ime::ReplayEdit boundary = direct.commit();
  CHECK_EQ(boundary.backspaces, size_t(0));
  CHECK_EQ(boundary.text, std::string(""));
  apply_replay(visible, boundary);
  CHECK_EQ(visible, std::string("đ"));

  feed('T', 0, "T", "đT");
  feed('i', 0, "i", "đTi");
  feed('e', 0, "e", "đTie");
  feed('e', 1, "ê", "đTiê");
  feed('n', 0, "n", "đTiên");
  feed('g', 0, "g", "đTiêng");
  feed('s', 3, "ếng", "đTiếng");  // keeping common prefix "Ti": 3 backspaces ("êng") then commit "ếng"

  boundary = direct.commit();
  CHECK_EQ(boundary.backspaces, size_t(0));
  CHECK_EQ(boundary.text, std::string(""));
  apply_replay(visible, boundary);
  CHECK_EQ(visible, std::string("đTiếng"));

  for (char key : std::string("window")) {
    ime::ReplayEdit edit;
    CHECK(direct.feed(static_cast<uint32_t>(key), &edit));
    CHECK_EQ(edit.backspaces, size_t(0));
    CHECK_EQ(edit.text, std::string(1, key));
    total_backspaces += edit.backspaces;
    apply_replay(visible, edit);
  }
  CHECK_EQ(visible, std::string("đTiếngwindow"));
  boundary = direct.commit();
  CHECK_EQ(boundary.backspaces, size_t(0));
  apply_replay(visible, boundary);

  // Exactly 5 BackSpaces across "dd" (1) + "Tieengs" (4) + "window" (0)
  CHECK_EQ(total_backspaces, size_t(5));
}

YUI_TEST(ime, direct_mode_backspace_restore_and_boundaries) {
  ime::DirectComposition direct;
  std::string visible;
  for (char key : std::string("Tieengs")) {
    ime::ReplayEdit edit;
    CHECK(direct.feed(static_cast<uint32_t>(key), &edit));
    apply_replay(visible, edit);
  }
  CHECK_EQ(visible, std::string("Tiếng"));

  ime::ReplayEdit edit;
  CHECK(direct.backspace(&edit));
  CHECK_EQ(edit.backspaces, size_t(3));
  CHECK_EQ(edit.text, std::string("êng"));
  apply_replay(visible, edit);
  CHECK_EQ(visible, std::string("Tiêng"));
  CHECK(direct.feed('s', &edit));
  CHECK_EQ(edit.backspaces, size_t(3));
  CHECK_EQ(edit.text, std::string("ếng"));
  apply_replay(visible, edit);
  CHECK_EQ(visible, std::string("Tiếng"));

  edit = direct.restore();
  CHECK_EQ(edit.backspaces, size_t(3));
  CHECK_EQ(edit.text, std::string("eengs"));
  apply_replay(visible, edit);
  CHECK_EQ(visible, std::string("Tieengs"));
  CHECK(direct.empty());

  // A boundary forgets the active suffix: the next edit cannot backspace into
  // the preceding word or punctuation that the IBus adapter forwards.
  CHECK(direct.feed('d', &edit));
  CHECK_EQ(edit.backspaces, size_t(0));
  apply_replay(visible, edit);
  direct.reset();
  CHECK(direct.feed('a', &edit));
  CHECK_EQ(edit.backspaces, size_t(0));
  CHECK_EQ(edit.text, std::string("a"));
  apply_replay(visible, edit);
  CHECK_EQ(visible, std::string("Tieengsda"));
}

YUI_TEST(ime, direct_mode_vni_replay_and_english_restore) {
  ime::DirectComposition direct(ime::Method::Vni);
  std::string visible;
  ime::ReplayEdit edit;

  CHECK(direct.feed('a', &edit));
  CHECK_EQ(edit.backspaces, size_t(0));
  CHECK_EQ(edit.text, std::string("a"));
  apply_replay(visible, edit);
  CHECK(direct.feed('6', &edit));
  CHECK_EQ(edit.backspaces, size_t(1));
  CHECK_EQ(edit.text, std::string("â"));
  apply_replay(visible, edit);
  CHECK(direct.feed('1', &edit));
  CHECK_EQ(edit.backspaces, size_t(1));
  CHECK_EQ(edit.text, std::string("ấ"));
  apply_replay(visible, edit);
  CHECK_EQ(visible, std::string("ấ"));

  edit = direct.commit();
  CHECK_EQ(edit.backspaces, size_t(0));
  CHECK_EQ(edit.text, std::string(""));

  for (char key : std::string("window")) {
    CHECK(direct.feed(static_cast<uint32_t>(key), &edit));
    apply_replay(visible, edit);
  }
  CHECK_EQ(visible, std::string("ấwindow"));
  edit = direct.commit();
  CHECK_EQ(edit.backspaces, size_t(0));
  CHECK_EQ(edit.text, std::string(""));
  CHECK_EQ(direct.method(), ime::Method::Vni);

  // Direct rendering must retain the same conservative English allowlist as
  // preedit mode, including terms whose letters otherwise activate Telex.
  for (const std::string word : {"word", "window", "software", "user", "linux"}) {
    ime::DirectComposition english;
    std::string literal;
    std::string english_visible;
    for (char key : word) {
      literal.push_back(key);
      CHECK(english.feed(static_cast<uint32_t>(key), &edit));
      CHECK_EQ(edit.backspaces, size_t(0));
      CHECK_EQ(edit.text, std::string(1, key));
      apply_replay(english_visible, edit);
      CHECK_EQ(english_visible, literal);
    }
    edit = english.commit();
    CHECK_EQ(edit.backspaces, size_t(0));
    CHECK_EQ(edit.text, std::string(""));
    CHECK_EQ(english_visible, word);
  }
}

YUI_TEST(ime, direct_mode_random_replay_and_common_prefix_invariants) {
  // Test at least 200 random / diverse typing sequences in both Telex and VNI,
  // verifying that keeping common prefix always yields identical text to naive
  // full-word replay at EVERY step, with total backspaces <= naive replay.
  const std::vector<std::string> telex_corpus = {
      "dd", "DD", "ddw", "DDawng", "dduwowngf", "truwowngf", "phuwowng", "thuwowng",
      "nguwowif", "buwowsc", "nwowsc", "luwowjt", "chuwowng", "mowsi", "rowif",
      "ddoofng", "hoaf", "toanf", "khoai", "tuoofi", "quar", "gias", "cawps",
      "mawms", "tats", "nats", "cacs", "VIEETJ", "TIEENGS", "DDUWOWNGF",
      "TRUWOWNGF", "DDang", "DDooj", "Hoangf", "Nguyeexn", "Trar", "ddur",
      "ddax", "ddaj", "khoo", "quees", "thuyeenf", "khoer", "nhanh", "chuyeern",
      "nghieeng", "quyeesn", "xuyeen", "ddieeur", "ddieemr", "chuwas", "chuwa",
      "tuwowng", "ddawsng", "thawsng", "chawsng", "lawswng", "khasch", "trrawng",
      "muwowsi", "twrowif", "nguwowi", "ddawjt", "ngawjt", "bieets", "tieecs",
      "thieets", "kieems", "lieems", "chieecs", "bieern", "khieens", "lieenf",
      "chieenf", "mieens", "tieens", "thieens", "ddieens", "khoo", "xoof",
      "choof", "hooj", "dooj", "soos", "toos", "loos", "phoos", "zoos", "wo",
      "wa", "we", "wi", "w", "Tieengs", "vieetj", "nam", "thaif", "duwowng",
      "yeue", "uoong", "thuoocj", "khuyeens", "maix", "cuowis", "tuowri"
  };

  const std::vector<std::string> vni_corpus = {
      "d9", "D9", "d9u7o7ng2", "tru7o7ng2", "phu7o7ng", "thu7o7ng", "ngu7o7i2",
      "bu7o7c1", "nu7o7c1", "lu7o7t5", "chu7o7ng", "mo7i1", "ro7i2", "d9o62ng",
      "hoa2", "toan2", "khoai", "tuo6i2", "qua3", "gia1", "ca8p1", "ma8m1",
      "ta1t", "na1t", "ca1c", "VIET65", "TIENG1", "D9U7O7NG2", "TRU7O7NG2",
      "D9ang", "D9o65", "Hoang2", "Nguyen4", "Tra3", "d9u3", "d9a4", "d9a5",
      "kho6", "que1", "thuyen2", "kho3", "nhanh", "chuyen3", "nghieng",
      "quyen1", "xuyen", "d9ieu3", "d9iem3", "chu7a1", "chu7a", "tu7o7ng",
      "d9a8ng1", "tha8ng1", "cha8ng1", "la8ng1", "kha1ch", "tra8ng", "mu7o7i1",
      "tro7i2", "ngu7o7i", "d9a8t5", "nga8t5", "biet1", "tiec1", "thiet1",
      "kiem1", "liem1", "chiec1", "bien3", "khien1", "lien2", "chien2",
      "mien1", "tien1", "thien1", "d9ien1", "viet65", "nam"
  };

  size_t tested_count = 0;

  auto run_sequence = [&](ime::Method method, const std::string& seq, bool test_backspace) {
    ime::DirectComposition direct(method);
    ime::Composition naive_comp(method);
    std::string visible_direct;
    std::string visible_naive;
    std::string rendered_naive;
    size_t total_bs_direct = 0;
    size_t total_bs_naive = 0;

    for (size_t i = 0; i < seq.size(); ++i) {
      const uint32_t key = static_cast<uint8_t>(seq[i]);
      ime::ReplayEdit direct_edit;
      bool ok_d = direct.feed(key, &direct_edit);
      bool ok_n = naive_comp.feed(key);
      CHECK_EQ(ok_d, ok_n);
      if (ok_d) {
        apply_replay(visible_direct, direct_edit);
        total_bs_direct += direct_edit.backspaces;

        ime::ReplayEdit naive_edit;
        naive_edit.backspaces = text::length(rendered_naive);
        naive_edit.text = naive_comp.preedit();
        apply_replay(visible_naive, naive_edit);
        rendered_naive = naive_comp.preedit();
        total_bs_naive += naive_edit.backspaces;

        CHECK_EQ(visible_direct, visible_naive);
      }

      // Occasionally test a backspace mid-composition if requested
      if (test_backspace && i == seq.size() / 2 && !direct.empty()) {
        ime::ReplayEdit bs_direct;
        bool bs_d = direct.backspace(&bs_direct);
        bool bs_n = naive_comp.backspace();
        CHECK_EQ(bs_d, bs_n);
        if (bs_d) {
          apply_replay(visible_direct, bs_direct);
          total_bs_direct += bs_direct.backspaces;

          ime::ReplayEdit bs_naive;
          bs_naive.backspaces = text::length(rendered_naive);
          bs_naive.text = naive_comp.preedit();
          apply_replay(visible_naive, bs_naive);
          rendered_naive = naive_comp.preedit();
          total_bs_naive += bs_naive.backspaces;

          CHECK_EQ(visible_direct, visible_naive);
        }
      }
    }

    // Commit boundary
    ime::ReplayEdit commit_direct = direct.commit();
    apply_replay(visible_direct, commit_direct);
    total_bs_direct += commit_direct.backspaces;

    ime::ReplayEdit commit_naive;
    std::string committed_n = naive_comp.commit();
    if (committed_n != rendered_naive) {
      commit_naive.backspaces = text::length(rendered_naive);
      commit_naive.text = committed_n;
    }
    apply_replay(visible_naive, commit_naive);
    total_bs_naive += commit_naive.backspaces;

    CHECK_EQ(visible_direct, visible_naive);
    CHECK(total_bs_direct <= total_bs_naive);
    ++tested_count;
  };

  // 1. Run all curated real words for Telex (with and without mid-typing backspace)
  for (const auto& w : telex_corpus) {
    run_sequence(ime::Method::Telex, w, false);
    run_sequence(ime::Method::Telex, w, true);
  }

  // 2. Run all curated real words for VNI (with and without mid-typing backspace)
  for (const auto& w : vni_corpus) {
    run_sequence(ime::Method::Vni, w, false);
    run_sequence(ime::Method::Vni, w, true);
  }

  // 3. Deterministic PRNG sequences (Telex and VNI)
  uint32_t rng = 0x12345678;
  auto next_rng = [&]() {
    rng = rng * 1664525u + 1013904223u;
    return rng;
  };

  const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
  const size_t charset_len = sizeof(charset) - 1;

  for (int iter = 0; iter < 100; ++iter) {
    std::string seq;
    size_t len = 3 + (next_rng() % 8);
    for (size_t l = 0; l < len; ++l) {
      seq.push_back(charset[next_rng() % charset_len]);
    }
    ime::Method m = (next_rng() % 2 == 0) ? ime::Method::Telex : ime::Method::Vni;
    run_sequence(m, seq, (next_rng() % 2 == 1));
  }

  CHECK(tested_count >= size_t(200));
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
