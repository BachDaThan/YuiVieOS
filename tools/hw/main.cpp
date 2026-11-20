// yui-hw — Trợ lý phần cứng (P6.31).
//
//   yui-hw                    bảng tiếng Việt: thiết bị, hạng dự kiến/đã đo, hướng dẫn
//   yui-hw --json             JSON (không có dấu thời gian ⇒ quét lại cho cùng kết quả)
//   yui-hw --write PATH       ghi JSON ra tệp CỤC BỘ PATH (không gửi đi đâu — P7.5)
//   yui-hw --root DIR         quét cây sysfs/procfs khác (bản chụp, test)
//   yui-hw --hwdb PATH        bảng thiết bị (mặc định: $YUI_HWDB, docs/hwdb.tsv,
//                             /usr/local/share/yui/hwdb.tsv, /usr/share/yui/hwdb.tsv)
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "yui/hw.hpp"

namespace {

bool slurp(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool json_out = false;
  std::string write_path, root = "/", hwdb_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--json") json_out = true;
    else if (a == "--write" && i + 1 < argc) write_path = argv[++i];
    else if (a == "--root" && i + 1 < argc) root = argv[++i];
    else if (a == "--hwdb" && i + 1 < argc) hwdb_path = argv[++i];
    else if (a == "--help" || a == "-h") {
      std::printf("yui-hw [--json] [--write PATH] [--root DIR] [--hwdb PATH]\n");
      return 0;
    } else {
      std::fprintf(stderr, "yui-hw: tham số không hợp lệ: %s\n", a.c_str());
      return 2;
    }
  }

  yui::hw::Hwdb db;
  if (!hwdb_path.empty()) {
    std::string text;
    if (!slurp(hwdb_path, text)) {
      std::fprintf(stderr, "yui-hw: không đọc được bảng %s\n", hwdb_path.c_str());
      return 1;
    }
    db = yui::hw::parse_hwdb(text);
  } else {
    std::string used;
    db = yui::hw::load_default_hwdb(&used);
    if (used.empty()) std::fprintf(stderr, "yui-hw: không thấy hwdb.tsv — mọi thiết bị sẽ ghi \"chưa đo\"\n");
  }
  const auto report = yui::hw::scan(root, db);
  const std::string doc = yui::hw::to_json(report).dump(true) + "\n";

  if (!write_path.empty()) {
    FILE* f = std::fopen(write_path.c_str(), "wb");
    if (!f) {
      std::fprintf(stderr, "yui-hw: không ghi được %s\n", write_path.c_str());
      return 1;
    }
    std::fwrite(doc.data(), 1, doc.size(), f);
    std::fclose(f);
    if (!json_out)
      std::fprintf(stderr, "yui-hw: đã ghi %s (chỉ trên máy này; muốn đóng góp thì bạn tự gửi PR)\n",
                   write_path.c_str());
  }
  if (json_out) std::fputs(doc.c_str(), stdout);
  else std::fputs(yui::hw::summary_vi(report).c_str(), stdout);
  return 0;
}
