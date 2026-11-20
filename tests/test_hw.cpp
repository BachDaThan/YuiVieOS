// P6.31 — yui-hw: xếp hạng A/B/C/D (§14.3b), bảng hwdb, quét cây sysfs giả,
// JSON ổn định, không có mã mạng.  Hạng ở đây là hạng DỰ KIẾN theo quy tắc;
// hạng ĐÃ ĐO chỉ đến từ hwdb (real/qemu).
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/hw.hpp"

namespace hw = yui::hw;

namespace {

hw::Platform plat_hp8000() {  // Core 2 (VT-x, không EPT), chưa thấy DMAR, BIOS
  hw::Platform p;
  p.cpu_flags_known = true;
  p.vmx = true;
  return p;
}
hw::Platform plat_modern() {
  hw::Platform p;
  p.cpu_flags_known = true;
  p.vmx = true;
  p.ept = true;
  p.iommu_table = "DMAR";
  p.uefi = true;
  p.has_xhci = true;
  return p;
}
hw::Platform plat_amd() {
  hw::Platform p;
  p.cpu_flags_known = true;
  p.svm = true;
  p.npt = true;
  p.iommu_table = "IVRS";
  p.uefi = true;
  return p;
}
hw::Platform plat_novirt() {
  hw::Platform p;
  p.cpu_flags_known = true;
  return p;
}
hw::Platform plat_unknown() { return hw::Platform{}; }  // aarch64: không có cờ VT-x

hw::Device pci(uint16_t v, uint16_t d, uint32_t cls) {
  hw::Device x;
  x.bus = hw::Bus::Pci;
  x.vendor = v;
  x.device = d;
  x.pci_class = cls;
  return x;
}
hw::Device usb(uint16_t v, uint16_t d, std::vector<uint8_t> cls) {
  hw::Device x;
  x.bus = hw::Bus::Usb;
  x.vendor = v;
  x.device = d;
  x.usb_classes = std::move(cls);
  return x;
}

struct Case {
  const char* what;
  hw::Device dev;
  hw::Platform plat;
  hw::Grade expected;
  hw::Category cat;
};

std::string read_all(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ── cây sysfs giả ──
void mkdirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur.push_back(path[i]);
    if (path[i] == '/' && cur.size() > 1) ::mkdir(cur.c_str(), 0755);
  }
  ::mkdir(path.c_str(), 0755);
}
void put(const std::string& path, const std::string& content) {
  mkdirs(path.substr(0, path.find_last_of('/')));
  std::ofstream f(path, std::ios::binary);
  f << content;
}
void link_to(const std::string& target, const std::string& path) {
  mkdirs(path.substr(0, path.find_last_of('/')));
  if (::symlink(target.c_str(), path.c_str()) != 0) std::perror("symlink");
}
std::string tmpdir(const char* tag) {
  std::string t = std::string("/tmp/yui-hw-") + tag + "-XXXXXX";
  std::vector<char> buf(t.begin(), t.end());
  buf.push_back('\0');
  const char* d = ::mkdtemp(buf.data());
  return d ? std::string(d) : std::string();
}
int rm_entry(const char* path, const struct stat*, int, struct FTW*) { return ::remove(path); }
// Xoá cây tạm khi ra khỏi phạm vi (chỉ dưới /tmp/yui-hw-).
struct TreeGuard {
  std::string path;
  ~TreeGuard() {
    if (path.rfind("/tmp/yui-hw-", 0) == 0) ::nftw(path.c_str(), rm_entry, 16, FTW_DEPTH | FTW_PHYS);
  }
};

void add_pci(const std::string& root, const std::string& slot, const char* v, const char* d, const char* cls,
             const char* driver) {
  const std::string b = root + "/sys/bus/pci/devices/" + slot;
  put(b + "/vendor", std::string(v) + "\n");
  put(b + "/device", std::string(d) + "\n");
  put(b + "/class", std::string(cls) + "\n");
  if (driver) link_to(std::string("../../../bus/pci/drivers/") + driver, b + "/driver");
}

// Máy giống HP Compaq 8000 Elite (mã PCI theo probe linux-hardware.org), SATA ở IDE.
std::string make_hp_tree(bool modern_fw) {
  const std::string r = tmpdir(modern_fw ? "m" : "hp");
  // Thứ tự tạo cố ý lộn xộn: quét phải tự sắp xếp.
  add_pci(r, "0000:00:1f.2", "0x8086", "0x3a00", "0x010180", "ata_piix");
  add_pci(r, "0000:00:02.0", "0x8086", "0x2e12", "0x030000", "i915");
  add_pci(r, "0000:00:00.0", "0x8086", "0x2e10", "0x060000", nullptr);
  add_pci(r, "0000:00:19.0", "0x8086", "0x10de", "0x020000", "e1000e");
  add_pci(r, "0000:00:1b.0", "0x8086", "0x3a6e", "0x040300", "snd_hda_intel");
  add_pci(r, "0000:00:1a.7", "0x8086", "0x3a6a", "0x0c0320", "ehci-pci");
  add_pci(r, "0000:00:03.0", "0x8086", "0x2e14", "0x078000", "mei_me");
  put(r + "/sys/bus/pci/devices/0000:00:1e.0/vendor", "garbage\n");  // hỏng ⇒ bỏ qua
  // USB: root hub (bỏ), bàn phím (HID), ổ USB, interface gộp vào cha.
  const std::string u = r + "/sys/bus/usb/devices/";
  put(u + "usb1/idVendor", "1d6b\n");
  put(u + "usb1/idProduct", "0002\n");
  put(u + "usb1/bDeviceClass", "09\n");
  put(u + "1-2/idVendor", "046d\n");
  put(u + "1-2/idProduct", "c31c\n");
  put(u + "1-2/bDeviceClass", "00\n");
  put(u + "1-2/product", "USB Keyboard\n");
  put(u + "1-2:1.0/bInterfaceClass", "03\n");
  put(u + "1-2:1.1/bInterfaceClass", "03\n");
  link_to("../../../bus/usb/drivers/usbhid", u + "1-2:1.0/driver");
  put(u + "1-1/idVendor", "0781\n");
  put(u + "1-1/idProduct", "5567\n");
  put(u + "1-1/bDeviceClass", "00\n");
  put(u + "1-1:1.0/bInterfaceClass", "08\n");
  // Webcam: interface tạo ngược thứ tự; lớp phải theo tên interface (1.0 rồi 1.1).
  put(u + "2-1/idVendor", "046d\n");
  put(u + "2-1/idProduct", "0825\n");
  put(u + "2-1/bDeviceClass", "ef\n");
  put(u + "2-1:1.3/bInterfaceClass", "01\n");
  put(u + "2-1:1.2/bInterfaceClass", "01\n");
  put(u + "2-1:1.1/bInterfaceClass", "0e\n");
  put(u + "2-1:1.0/bInterfaceClass", "0e\n");
  // CPU: Core 2 có vmx, không có ept; firmware/IOMMU tuỳ biến thể.
  if (modern_fw) {
    put(r + "/proc/cpuinfo",
        "processor\t: 0\nmodel name\t: Fake CPU\nflags\t\t: fpu sse2 vmx\nvmx flags\t: vnmi ept vpid\n\n"
        "processor\t: 1\nflags\t\t: fpu\n");
    put(r + "/sys/firmware/acpi/tables/DMAR", "x");
    mkdirs(r + "/sys/firmware/efi");
    mkdirs(r + "/sys/class/iommu/dmar0");
  } else {
    put(r + "/proc/cpuinfo",
        "processor\t: 0\nmodel name\t: Intel(R) Core(TM)2 Duo CPU     E8500  @ 3.16GHz\n"
        "flags\t\t: fpu sse2 ssse3 sse4_1 vmx est\n");
    put(r + "/sys/firmware/acpi/tables/APIC", "x");
  }
  // Ổ đĩa: sda SATA đĩa quay, sdb qua USB (liên kết chứa /usb), loop0 bị bỏ.
  mkdirs(r + "/sys/devices/pci0000:00/0000:00:1f.2/host0/block/sda");
  mkdirs(r + "/sys/devices/pci0000:00/0000:00:1a.7/usb1/1-1/host6/block/sdb");
  link_to("../devices/pci0000:00/0000:00:1f.2/host0/block/sda", r + "/sys/block/sda");
  link_to("../devices/pci0000:00/0000:00:1a.7/usb1/1-1/host6/block/sdb", r + "/sys/block/sdb");
  put(r + "/sys/block/sda/size", "488397168\n");
  put(r + "/sys/block/sda/queue/rotational", "1\n");
  put(r + "/sys/block/sda/removable", "0\n");
  put(r + "/sys/block/sdb/size", "60437492\n");
  put(r + "/sys/block/sdb/queue/rotational", "0\n");
  put(r + "/sys/block/sdb/removable", "1\n");
  mkdirs(r + "/sys/block/loop0");
  return r;
}

const hw::Item* find_item(const hw::Report& r, uint16_t v, uint16_t d) {
  for (const auto& it : r.items)
    if (it.device.vendor == v && it.device.device == d) return &it;
  return nullptr;
}

bool has_guide(const std::vector<std::string>& g, const std::string& needle) {
  for (const auto& s : g)
    if (s.find(needle) != std::string::npos) return true;
  return false;
}

}  // namespace

// ── 31 mô tả PCI/USB tổng hợp ⇒ hạng dự kiến đúng ──
YUI_TEST(hw, synthetic_descriptions_graded) {
  hw::Platform hp_noxhci = plat_hp8000();
  const std::vector<Case> cases = {
      {"GMA 4500 (HP 8000)", pci(0x8086, 0x2e12, 0x030000), plat_hp8000(), hw::Grade::C, hw::Category::Display},
      {"ICH10 HD Audio", pci(0x8086, 0x3a6e, 0x040300), plat_hp8000(), hw::Grade::A, hw::Category::Audio},
      {"82567LM-3 e1000e", pci(0x8086, 0x10de, 0x020000), plat_hp8000(), hw::Grade::A, hw::Category::Ethernet},
      {"SATA chế độ IDE", pci(0x8086, 0x3a00, 0x010180), plat_hp8000(), hw::Grade::D, hw::Category::Storage},
      {"SATA AHCI", pci(0x8086, 0x3a02, 0x010601), plat_hp8000(), hw::Grade::A, hw::Category::Storage},
      {"NVMe", pci(0x144d, 0xa808, 0x010802), plat_modern(), hw::Grade::A, hw::Category::Storage},
      {"Realtek 8168, có IOMMU", pci(0x10ec, 0x8168, 0x020000), plat_modern(), hw::Grade::B,
       hw::Category::Ethernet},
      {"Realtek 8168, không IOMMU", pci(0x10ec, 0x8168, 0x020000), plat_hp8000(), hw::Grade::D,
       hw::Category::Ethernet},
      {"Wi-Fi Intel, VT-x + DMAR", pci(0x8086, 0x2723, 0x028000), plat_modern(), hw::Grade::B,
       hw::Category::Wireless},
      {"Wi-Fi, CPU không ảo hoá", pci(0x8086, 0x2723, 0x028000), plat_novirt(), hw::Grade::D,
       hw::Category::Wireless},
      {"Wi-Fi, AMD-V + IVRS", pci(0x14c3, 0x7961, 0x028000), plat_amd(), hw::Grade::B, hw::Category::Wireless},
      {"Wi-Fi, chưa đọc được cờ CPU", pci(0x14e4, 0x43a0, 0x028000), plat_unknown(), hw::Grade::B,
       hw::Category::Wireless},
      {"xHCI", pci(0x8086, 0xa36d, 0x0c0330), plat_modern(), hw::Grade::A, hw::Category::UsbController},
      {"EHCI (USB 2.0)", pci(0x8086, 0x3a6a, 0x0c0320), plat_hp8000(), hw::Grade::C, hw::Category::UsbController},
      {"GPU rời lớp 3D", pci(0x10de, 0x1f91, 0x030200), plat_modern(), hw::Grade::B, hw::Category::Display},
      {"cầu nối host (hạ tầng)", pci(0x8086, 0x2e10, 0x060000), plat_hp8000(), hw::Grade::None,
       hw::Category::Infra},
      {"HECI (hạ tầng)", pci(0x8086, 0x2e14, 0x078000), plat_hp8000(), hw::Grade::None, hw::Category::Infra},
      {"virtio-net", pci(0x1af4, 0x1000, 0x020000), plat_novirt(), hw::Grade::A, hw::Category::Ethernet},
      {"AC'97", pci(0x8086, 0x2415, 0x040100), plat_modern(), hw::Grade::D, hw::Category::Audio},
      {"FireWire", pci(0x1106, 0x3044, 0x0c0010), plat_modern(), hw::Grade::D, hw::Category::Other},
      {"RAID của hãng", pci(0x8086, 0x2822, 0x010400), plat_modern(), hw::Grade::D, hw::Category::Storage},
      {"lớp PCI lạ (xử lý tín hiệu)", pci(0x8086, 0x1903, 0x118000), plat_modern(), hw::Grade::None,
       hw::Category::Other},
      {"bàn phím USB, có xHCI", usb(0x046d, 0xc31c, {0x03}), plat_modern(), hw::Grade::A, hw::Category::Input},
      {"bàn phím USB, chỉ EHCI", usb(0x046d, 0xc31c, {0x03}), hp_noxhci, hw::Grade::C, hw::Category::Input},
      {"ổ USB, VT-x không IOMMU", usb(0x0781, 0x5567, {0x08}), plat_hp8000(), hw::Grade::B,
       hw::Category::MassStorage},
      {"webcam UVC + micro", usb(0x046d, 0x0825, {0x0e, 0x01}), plat_modern(), hw::Grade::B,
       hw::Category::Camera},
      {"vân tay (lớp riêng hãng)", usb(0x06cb, 0x00bd, {0xff}), plat_modern(), hw::Grade::None,
       hw::Category::VendorSpecific},
      {"dongle Bluetooth, không ảo hoá", usb(0x8087, 0x0026, {0xe0}), plat_novirt(), hw::Grade::D,
       hw::Category::Bluetooth},
      {"hub USB", usb(0x05e3, 0x0610, {0x09}), plat_modern(), hw::Grade::None, hw::Category::Infra},
      {"USB-Ethernet CDC", usb(0x0bda, 0x8153, {0x02, 0x0a}), plat_hp8000(), hw::Grade::B,
       hw::Category::Ethernet},
      {"tai nghe USB Audio", usb(0x0d8c, 0x0014, {0x01, 0x03}), plat_modern(), hw::Grade::A,
       hw::Category::Input},  // có cả HID (nút âm lượng) ⇒ quy tắc HID đi trước
  };
  CHECK(cases.size() >= 20);
  const hw::Hwdb empty;
  int wrong = 0;
  for (const auto& c : cases) {
    const auto v = hw::classify(c.dev, c.plat, empty);
    const bool ok = v.expected == c.expected && v.category == c.cat;
    if (!ok) {
      ++wrong;
      std::printf("    sai: %s ⇒ %s/%s (cần %s/%s)\n", c.what, hw::grade_code(v.expected),
                  hw::category_vi(v.category), hw::grade_code(c.expected), hw::category_vi(c.cat));
    }
    CHECK_MSG(ok, c.what);
    // Hạng đã đo không bao giờ đến từ quy tắc.
    CHECK(v.grade == hw::Grade::None);
    CHECK_EQ(v.source, std::string("chưa đo"));
    CHECK(!v.expected_why.empty());
  }
  CHECK_EQ(wrong, 0);
}

YUI_TEST(hw, guidance_matches_platform) {
  const hw::Hwdb empty;
  auto v = hw::classify(pci(0x10ec, 0x8168, 0x020000), plat_hp8000(), empty);
  CHECK(has_guide(v.guide_vi, "Bật VT-d"));
  v = hw::classify(pci(0x14c3, 0x7961, 0x028000), [] {
    auto p = plat_amd();
    p.iommu_table.clear();
    return p;
  }(), empty);
  CHECK(v.expected == hw::Grade::D);
  CHECK(has_guide(v.guide_vi, "AMD-Vi"));
  v = hw::classify(pci(0x8086, 0x2723, 0x028000), plat_novirt(), empty);
  CHECK(has_guide(v.guide_vi, "Virtualization Technology"));
  CHECK(has_guide(v.guide_vi, "USB-Ethernet"));  // hướng dẫn Wi-Fi
  v = hw::classify(pci(0x8086, 0x3a00, 0x010180), plat_hp8000(), empty);
  CHECK(has_guide(v.guide_vi, "AHCI"));
  // Không EPT ⇒ B nhưng ghi rõ chậm.
  v = hw::classify(usb(0x0781, 0x5567, {0x08}), plat_hp8000(), empty);
  CHECK(v.expected == hw::Grade::B);
  CHECK(v.expected_why.find("EPT/NPT") != std::string::npos);
  v = hw::classify(usb(0x0781, 0x5567, {0x08}), plat_modern(), empty);
  CHECK(v.expected_why.find("EPT/NPT") == std::string::npos);

  // Hướng dẫn cấp máy.
  const auto r = hw::build(plat_hp8000(), {pci(0x8086, 0x2e12, 0x030000)}, empty);
  CHECK(has_guide(r.guide_vi, "DMAR"));
  CHECK(has_guide(r.guide_vi, "EPT/NPT"));
  CHECK(has_guide(r.guide_vi, "kiểu BIOS"));
  CHECK(has_guide(r.guide_vi, "1 thiết bị chưa đo"));
  CHECK(!has_guide(r.guide_vi, "máy ảo"));
  // build() tự nhận ra xHCI từ danh sách ⇒ bàn phím USB lên A; không có ⇒ C.
  hw::Platform no_flag = plat_hp8000();
  const auto rx = hw::build(no_flag, {pci(0x8086, 0xa36d, 0x0c0330), usb(0x046d, 0xc31c, {0x03})}, empty);
  CHECK(rx.platform.has_xhci);
  CHECK(rx.items.size() == 2 && rx.items[1].verdict.expected == hw::Grade::A);
  const auto rn = hw::build(no_flag, {pci(0x8086, 0x3a6a, 0x0c0320), usb(0x046d, 0xc31c, {0x03})}, empty);
  CHECK(!rn.platform.has_xhci);
  CHECK(rn.items.size() == 2 && rn.items[1].verdict.expected == hw::Grade::C);
  const auto r2 = hw::build(plat_modern(), {}, empty);
  CHECK(!has_guide(r2.guide_vi, "DMAR"));
  CHECK(!has_guide(r2.guide_vi, "kiểu BIOS"));
  CHECK(!has_guide(r2.guide_vi, "chưa đo"));
}

// ── hwdb ──
YUI_TEST(hw, hwdb_rules) {
  const std::string text =
      "# chú thích\n"
      "\n"
      "pci\t1234:1111\tC\tqemu\tQEMU stdvga\tCI run 1\n"
      "usb\t046d:c31c\tA\treal\tbàn phím\tmáy X, lệnh Y, gõ 100 phím đúng\r\n"
      "pci\t8086:2e12\t-\tchưa-đo\tGMA 4500\ttham khảo\n"
      "pci\t8086:2e13\tC\tchưa-đo\tchưa đo mà có hạng\tx\n"   // lỗi: chưa đo có hạng
      "pci\t8086:2e14\tB\treal\tthiếu bằng chứng\t \n"          // lỗi: không bằng chứng
      "pci\t8086:2e15\t-\treal\treal mà không hạng\tx\n"         // lỗi
      "pci\t8086:2e16\tE\tqemu\thạng lạ\tx\n"                   // lỗi
      "pci\t8086:2E1\tC\tqemu\tmã sai\tx\n"                     // lỗi
      "isa\t8086:2e17\tC\tqemu\tbus lạ\tx\n"                    // lỗi
      "pci\t8086:2e18\tC\tđồn\tnguồn lạ\tx\n"                   // lỗi
      "pci\t8086:2e19\tC\tqemu\tthiếu cột\n"                    // lỗi
      "pci\t1234:1111\tD\tqemu\ttrùng\tx\n";                    // lỗi: trùng
  const auto db = hw::parse_hwdb(text);
  CHECK_EQ(db.rows.size(), size_t(3));
  CHECK_EQ(db.errors.size(), size_t(9));
  const auto* e = db.find(hw::Bus::Usb, 0x046d, 0xc31c);
  CHECK(e != nullptr && e->grade == hw::Grade::A && e->source == "real");
  CHECK(db.find(hw::Bus::Pci, 0x046d, 0xc31c) == nullptr);  // khác bus
  CHECK(db.find(hw::Bus::Pci, 0x8086, 0x2e13) == nullptr);  // dòng lỗi không vào bảng

  // Đã đo ⇒ grade + nguồn + bằng chứng; chưa đo ⇒ không hạng, chỉ tên.
  auto v = hw::classify(pci(0x1234, 0x1111, 0x030000), plat_modern(), db);
  CHECK(v.grade == hw::Grade::C);
  CHECK_EQ(v.source, std::string("qemu"));
  CHECK_EQ(v.evidence, std::string("CI run 1"));
  v = hw::classify(usb(0x046d, 0xc31c, {0x03}), plat_modern(), db);
  CHECK(v.grade == hw::Grade::A);
  CHECK_EQ(v.source, std::string("real"));
  v = hw::classify(pci(0x8086, 0x2e12, 0x030000), plat_hp8000(), db);
  CHECK(v.grade == hw::Grade::None);
  CHECK_EQ(v.source, std::string("chưa đo"));
  CHECK_EQ(v.known_name, std::string("GMA 4500"));
  CHECK(v.expected == hw::Grade::C);

  const auto r = hw::build(plat_hp8000(), {pci(0x8086, 0x2e12, 0x030000)}, db);
  CHECK_EQ(r.hwdb_errors.size(), size_t(9));
  CHECK(hw::to_json(r)["hwdb_errors"].size() == 9);
}

YUI_TEST(hw, seed_hwdb_is_lawful) {
  const std::string text = read_all(std::string(YUI_SOURCE_DIR) + "/docs/hwdb.tsv");
  CHECK(!text.empty());
  const auto db = hw::parse_hwdb(text);
  for (const auto& e : db.errors) std::printf("    hwdb: %s\n", e.c_str());
  CHECK_EQ(db.errors.size(), size_t(0));
  CHECK(db.rows.size() >= 14);
  size_t measured = 0;
  for (const auto& r : db.rows) {
    if (r.grade != hw::Grade::None) {
      ++measured;
      CHECK(r.source == "real" || r.source == "qemu");
      CHECK(!r.note.empty());
    } else {
      CHECK_EQ(r.source, std::string("chưa-đo"));
    }
  }
  // Thiết bị HP Compaq 8000 Elite có trong bảng nhưng CHƯA đo trên máy bạn.
  for (uint16_t id : {0x2e12, 0x3a6e, 0x10de, 0x3a00, 0x3a02}) {
    const auto* e = db.find(hw::Bus::Pci, 0x8086, id);
    CHECK(e != nullptr && e->grade == hw::Grade::None);
  }
  CHECK(measured >= 1);  // QEMU stdvga (splash VBE trên CI)
}

// ── quét cây sysfs giả ──
YUI_TEST(hw, scan_fake_sysfs_hp8000) {
  const std::string root = make_hp_tree(false);
  const TreeGuard guard{root};
  CHECK(!root.empty());
  const auto db = hw::parse_hwdb(read_all(std::string(YUI_SOURCE_DIR) + "/docs/hwdb.tsv"));
  const auto r = hw::scan(root, db);
  const auto& p = r.platform;
  CHECK(p.cpu_flags_known);
  CHECK(p.vmx && !p.svm && !p.ept && !p.npt && !p.hypervisor);
  CHECK(p.iommu_table.empty());
  CHECK(!p.iommu_active);
  CHECK(!p.uefi);
  CHECK(!p.has_xhci);
  CHECK(p.cpu_model.find("E8500") != std::string::npos);
  CHECK_EQ(p.disks.size(), size_t(2));  // loop0 bị bỏ
  if (p.disks.size() == 2) {
    CHECK_EQ(p.disks[0].name, std::string("sda"));
    CHECK_EQ(p.disks[0].kind, std::string("SATA/SCSI"));
    CHECK(p.disks[0].rotational && !p.disks[0].removable);
    CHECK_EQ(p.disks[0].size_bytes, uint64_t(488397168) * 512u);
    CHECK_EQ(p.disks[1].kind, std::string("USB"));
    CHECK(p.disks[1].removable && !p.disks[1].rotational);
  }
  // 7 PCI hợp lệ (1 hỏng bị bỏ) + 3 USB (root hub bị bỏ).
  CHECK_EQ(r.items.size(), size_t(10));
  if (r.items.size() == 10) {
    CHECK_EQ(r.items[0].device.slot, std::string("0000:00:00.0"));
    CHECK_EQ(r.items[6].device.slot, std::string("0000:00:1f.2"));
    CHECK_EQ(r.items[7].device.slot, std::string("1-1"));
    CHECK_EQ(r.items[8].device.slot, std::string("1-2"));
  }
  const auto* igd = find_item(r, 0x8086, 0x2e12);
  CHECK(igd && igd->verdict.expected == hw::Grade::C && igd->verdict.grade == hw::Grade::None);
  CHECK(igd && igd->device.linux_driver == "i915");
  CHECK(igd && igd->verdict.known_name.find("GMA 4500") != std::string::npos);
  const auto* nic = find_item(r, 0x8086, 0x10de);
  CHECK(nic && nic->verdict.expected == hw::Grade::A && nic->device.linux_driver == "e1000e");
  const auto* ide = find_item(r, 0x8086, 0x3a00);
  CHECK(ide && ide->verdict.expected == hw::Grade::D);
  const auto* ehci = find_item(r, 0x8086, 0x3a6a);
  CHECK(ehci && ehci->verdict.expected == hw::Grade::C && ehci->verdict.known_name.empty());
  const auto* kbd = find_item(r, 0x046d, 0xc31c);
  CHECK(kbd && kbd->verdict.expected == hw::Grade::C);  // không xHCI ⇒ qua giả lập PS/2
  CHECK(kbd && kbd->device.usb_classes.size() == 1 && kbd->device.name == "USB Keyboard");
  CHECK(kbd && kbd->device.linux_driver == "usbhid");
  const auto* cam = find_item(r, 0x046d, 0x0825);
  CHECK(cam && cam->device.usb_classes == std::vector<uint8_t>({0x0e, 0x01}));  // 0xef bị bỏ, không trùng
  CHECK(cam && cam->verdict.category == hw::Category::Camera);
  const auto* stick = find_item(r, 0x0781, 0x5567);
  CHECK(stick && stick->verdict.expected == hw::Grade::B);  // USB không cần IOMMU
  CHECK(has_guide(r.guide_vi, "VT-d"));
  CHECK(has_guide(r.guide_vi, "AHCI"));
  CHECK(has_guide(r.guide_vi, "USB 2.0"));
  CHECK(r.hwdb_errors.empty());
  const std::string sum = hw::summary_vi(r);
  CHECK(sum.find("dự kiến") != std::string::npos);
  CHECK(sum.find('%') == std::string::npos);  // không công bố phần trăm
}

YUI_TEST(hw, scan_fake_sysfs_modern_fw) {
  const std::string root = make_hp_tree(true);
  const TreeGuard guard{root};
  const auto r = hw::scan(root + "///", hw::Hwdb{});
  const auto& p = r.platform;
  CHECK(p.vmx && p.ept);  // "vmx flags: ... ept" của nhân mới
  CHECK_EQ(p.cpu_model, std::string("Fake CPU"));
  CHECK_EQ(p.iommu_table, std::string("DMAR"));
  CHECK(p.iommu_active);
  CHECK(p.uefi);
  CHECK(!has_guide(r.guide_vi, "kiểu BIOS"));
  CHECK(!has_guide(r.guide_vi, "Không thấy bảng ACPI"));
  // Không hwdb ⇒ không ai có tên/hạng đo.
  for (const auto& it : r.items) {
    CHECK(it.verdict.grade == hw::Grade::None);
    CHECK(it.verdict.known_name.empty());
  }
  // Gốc không tồn tại ⇒ báo cáo rỗng, không lỗi.
  const auto none = hw::scan("/khong/co/thu/muc/nay", hw::Hwdb{});
  CHECK(none.items.empty());
  CHECK(!none.platform.cpu_flags_known);
  CHECK(none.platform.disks.empty());
}

// ── JSON ổn định ──
YUI_TEST(hw, json_is_stable) {
  const std::string root = make_hp_tree(false);
  const TreeGuard guard{root};
  const auto db = hw::parse_hwdb(read_all(std::string(YUI_SOURCE_DIR) + "/docs/hwdb.tsv"));
  const std::string a = hw::to_json(hw::scan(root, db)).dump(true);
  const std::string b = hw::to_json(hw::scan(root, db)).dump(true);
  CHECK_EQ(a, b);
  // Thứ tự đầu vào không ảnh hưởng đầu ra.
  std::vector<hw::Device> devs = {pci(0x8086, 0x2e12, 0x030000), usb(0x046d, 0xc31c, {0x03}),
                                  pci(0x8086, 0x3a6e, 0x040300)};
  devs[0].slot = "0000:00:02.0";
  devs[1].slot = "1-2";
  devs[2].slot = "0000:00:1b.0";
  std::vector<hw::Device> rev(devs.rbegin(), devs.rend());
  const std::string x = hw::to_json(hw::build(plat_hp8000(), devs, db)).dump();
  const std::string y = hw::to_json(hw::build(plat_hp8000(), rev, db)).dump();
  CHECK_EQ(x, y);
  // Nội dung: đọc lại được, đúng khoá.
  const auto j = yui::json::parse_or_null(a);
  CHECK_EQ(j["schema"].as_string(), std::string("yui-hw/1"));
  CHECK_EQ(j["platform"]["firmware"].as_string(), std::string("BIOS"));
  CHECK_EQ(j["devices"].size(), size_t(10));
  const auto& first = j["devices"].arr()[0];
  CHECK_EQ(first["bus"].as_string(), std::string("pci"));
  CHECK_EQ(first["id"].as_string(), std::string("8086:2e10"));
  CHECK_EQ(first["class"].as_string(), std::string("060000"));
  CHECK_EQ(first["expected"].as_string(), std::string("-"));
  const auto& last = j["devices"].arr()[8];  // 1-2 (bàn phím); 2-1 là webcam
  CHECK_EQ(last["bus"].as_string(), std::string("usb"));
  CHECK_EQ(last["classes"].arr()[0].as_string(), std::string("03"));
  CHECK_EQ(last["source"].as_string(), std::string("chưa đo"));
  CHECK(a.find("time") == std::string::npos);  // không có dấu thời gian
}

// ── P7.5: không có mã mạng trong yui-hw ──
YUI_TEST(hw, no_network_code) {
  for (const char* f : {"/kernel-services/hw.cpp", "/tools/hw/main.cpp", "/include/yui/hw.hpp"}) {
    const std::string src = read_all(std::string(YUI_SOURCE_DIR) + f);
    CHECK(!src.empty());
    for (const char* bad : {"socket(", "connect(", "sys/socket.h", "netdb.h", "curl", "getaddrinfo"})
      CHECK_MSG(src.find(bad) == std::string::npos, (std::string(f) + " chứa " + bad).c_str());
  }
}

YUI_TEST_MAIN("test_hw")
