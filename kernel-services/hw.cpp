// yui-hw — quét + xếp hạng phần cứng (P6.31).  Chỉ đọc tệp dưới `root`;
// không ghi gì, không mở socket.  Mọi tệp đều tuỳ chọn: thiếu thì bỏ qua.
#include "yui/hw.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "yui/text.hpp"

namespace yui::hw {
namespace {

std::string read_small(const std::string& path, size_t limit = 4096) {
  std::ifstream f(path);
  if (!f) return "";
  std::string s;
  s.resize(limit);
  f.read(&s[0], std::streamsize(limit));
  s.resize(size_t(f.gcount()));
  return yui::text::trim(s);
}

bool exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> out;
  DIR* d = ::opendir(path.c_str());
  if (!d) return out;
  while (const dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    out.push_back(n);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());  // thứ tự ổn định, không phụ thuộc readdir
  return out;
}

std::string link_target(const std::string& path) {
  char buf[4096];
  const ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
  if (n <= 0) return "";
  return std::string(buf, size_t(n));
}

std::string basename_of(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

// "0x8086" / "8086" ⇒ giá trị; lỗi ⇒ false.
bool parse_hex(const std::string& s0, uint32_t max, uint32_t& out) {
  std::string s = yui::text::trim(s0);
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
  if (s.empty() || s.size() > 8) return false;
  uint32_t v = 0;
  for (char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = v * 16 + uint32_t(d);
  }
  if (v > max) return false;
  out = v;
  return true;
}

std::string hex4(uint16_t v) {
  char b[8];
  std::snprintf(b, sizeof b, "%04x", unsigned(v));
  return b;
}
std::string hex6(uint32_t v) {
  char b[12];
  std::snprintf(b, sizeof b, "%06x", unsigned(v & 0xffffff));
  return b;
}
std::string hex2(uint8_t v) {
  char b[4];
  std::snprintf(b, sizeof b, "%02x", unsigned(v));
  return b;
}

const char* bus_name(Bus b) { return b == Bus::Pci ? "pci" : "usb"; }

// ── Quy tắc ─────────────────────────────────────────────────────────────

enum class Path {
  Native,      // A dự kiến: driver tự viết đã có task
  Basic,       // C: chỉ cơ bản
  PassPci,     // B nếu VT-x/AMD-V + IOMMU
  PassUsb,     // B nếu VT-x/AMD-V (USB qua P3.25, không cần IOMMU)
  Unsupported, // D
  Infra,       // hạ tầng chipset, không cần hạng
  Unknown,     // không suy ra được ⇒ không có hạng dự kiến
};

struct Rule {
  Category cat = Category::Other;
  Path path = Path::Unknown;
  std::string why;
  std::vector<std::string> guide;
};

bool is_known_e1000_family(uint16_t vendor, uint16_t device) {
  if (vendor != 0x8086) return false;
  // Chỉ những mã mình biết chắc: 82540EM (QEMU `e1000`), 82574L (QEMU
  // `e1000e`), 82567LM-3 (máy HP Compaq 8000 Elite theo linux-hardware.org).
  return device == 0x100e || device == 0x10d3 || device == 0x10de;
}

bool usb_has(const Device& d, uint8_t cls) {
  return std::find(d.usb_classes.begin(), d.usb_classes.end(), cls) != d.usb_classes.end();
}

Rule pci_rule(const Device& d) {
  Rule r;
  const uint8_t base = uint8_t(d.pci_class >> 16), sub = uint8_t(d.pci_class >> 8),
                pif = uint8_t(d.pci_class);
  if (d.vendor == 0x1af4) {  // VirtIO (máy ảo)
    r.cat = base == 0x03 ? Category::Display
            : base == 0x02 ? Category::Ethernet
            : base == 0x01 ? Category::Storage
            : base == 0x09 ? Category::Input
                           : Category::Other;
    r.path = Path::Native;
    r.why = "VirtIO — driver tự viết (P6.3)";
    return r;
  }
  switch (base) {
    case 0x01:
      r.cat = Category::Storage;
      if (sub == 0x08) {
        r.path = Path::Native;
        r.why = "NVMe — driver tự viết (P6.5)";
      } else if (sub == 0x06 && pif == 0x01) {
        r.path = Path::Native;
        r.why = "SATA AHCI — driver tự viết (P6.5)";
      } else if (sub == 0x01) {
        r.path = Path::Unsupported;
        r.why = "SATA đang ở chế độ IDE: không có task driver IDE";
        r.guide.push_back(
            "Đổi chế độ SATA từ IDE sang AHCI trong BIOS ⇒ ổ đĩa lên hạng A dự kiến (P6.5). "
            "Nếu đã cài Windows trên ổ này, làm theo hướng dẫn của Microsoft trước khi đổi.");
      } else if (sub == 0x04) {
        r.path = Path::Unsupported;
        r.why = "ổ đĩa ở chế độ RAID của hãng: không có driver";
        r.guide.push_back("Đổi chế độ lưu trữ từ RAID sang AHCI trong BIOS (cẩn thận nếu đang dùng RAID thật).");
      } else {
        r.path = Path::Unknown;
        r.why = "bộ điều khiển lưu trữ lớp " + hex2(sub) + ": chưa có quy tắc";
      }
      return r;
    case 0x02:
      if (sub == 0x00) {
        r.cat = Category::Ethernet;
        if (is_known_e1000_family(d.vendor, d.device)) {
          r.path = Path::Native;
          r.why = "mạng dây Intel họ e1000/e1000e — driver tự viết (P6.7, P6.34)";
        } else {
          r.path = Path::PassPci;
          r.why = "mạng dây chip khác (ví dụ Realtek): chưa có task driver ⇒ qua micro-VM Linux (P3.25/P3.26)";
        }
      } else {
        r.cat = Category::Wireless;  // 0x0280: thẻ Wi-Fi thường khai lớp này
        r.path = Path::PassPci;
        r.why = "Wi-Fi/mạng không dây: không tự viết (P6.7) ⇒ qua net-vm (micro-VM Linux)";
        r.guide.push_back(
            "Wi-Fi không có driver trên nhân riêng: cắm dây mạng hoặc USB-Ethernet; dongle Wi-Fi USB "
            "cũng đi qua micro-VM (hạng B, cần VT-x/AMD-V). Chip cụ thể: chưa đo — xem docs/hwdb.tsv.");
      }
      return r;
    case 0x03:
      r.cat = Category::Display;
      if (sub == 0x02) {
        r.path = Path::PassPci;
        r.why = "GPU rời không có đầu ra khung hình (lớp 3D): chỉ dùng được khi cấp thẳng cho micro-VM (P6.28)";
      } else {
        r.path = Path::Basic;
        r.why = "khung hình thường qua VBE/GOP/simple-framebuffer (P2.16), không tăng tốc 3D (P6.28)";
      }
      return r;
    case 0x04:
      r.cat = Category::Audio;
      if (sub == 0x03) {
        r.path = Path::Native;
        r.why = "Intel HD Audio — driver tự viết (P6.32); codec thật phải thử trên máy thật";
      } else if (sub == 0x01) {
        r.path = Path::Unsupported;
        r.why = "âm thanh AC'97/đời cũ: không có task";
      } else {
        r.cat = Category::Other;
        r.path = Path::PassPci;
        r.why = "thiết bị đa phương tiện (thu hình/TV): qua micro-VM";
      }
      return r;
    case 0x0c:
      if (sub == 0x03) {
        r.cat = Category::UsbController;
        if (pif == 0x30) {
          r.path = Path::Native;
          r.why = "USB 3 xHCI — driver tự viết (P6.6)";
        } else if (pif == 0x00 || pif == 0x10 || pif == 0x20) {
          r.path = Path::Basic;
          r.why = std::string("USB ") + (pif == 0x20 ? "2.0 EHCI" : pif == 0x10 ? "1.1 OHCI" : "1.1 UHCI") +
                  ": P6.6 chỉ có xHCI ⇒ bàn phím/chuột USB đi qua giả lập PS/2 của BIOS (P6.33)";
          r.guide.push_back(
              "Máy chỉ có USB 2.0 (EHCI/UHCI): dùng bàn phím/chuột PS/2 nếu có cổng, hoặc bật "
              "\"USB Legacy Support\" trong BIOS; USB thường (ổ, dongle) chưa có driver gốc.");
        } else {
          r.path = Path::Unknown;
          r.why = "bộ điều khiển USB kiểu " + hex2(pif) + ": chưa có quy tắc";
        }
        return r;
      }
      if (sub == 0x00) {
        r.cat = Category::Other;
        r.path = Path::Unsupported;
        r.why = "FireWire (IEEE 1394): không có task";
        return r;
      }
      r.cat = Category::Infra;
      r.path = Path::Infra;
      r.why = "hạ tầng chipset (SMBus/bus nối tiếp)";
      return r;
    case 0x0d:
      r.cat = sub == 0x11 ? Category::Bluetooth : Category::Wireless;
      r.path = Path::PassPci;
      r.why = "không dây (Bluetooth/Wi-Fi) ⇒ qua net-vm (P6.46)";
      return r;
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0b:
      r.cat = Category::Infra;
      r.path = Path::Infra;
      r.why = "hạ tầng chipset (cầu nối/bộ nhớ/ngoại vi hệ thống)";
      return r;
    default:
      r.cat = Category::Other;
      r.path = Path::Unknown;
      r.why = "lớp PCI " + hex6(d.pci_class) + ": chưa có quy tắc — cần đo";
      return r;
  }
}

Rule usb_rule(const Device& d, bool has_xhci) {
  Rule r;
  if (usb_has(d, 0x09)) {
    r.cat = Category::Infra;
    r.path = Path::Infra;
    r.why = "hub USB";
    return r;
  }
  if (usb_has(d, 0x03)) {
    r.cat = Category::Input;
    if (has_xhci) {
      r.path = Path::Native;
      r.why = "USB HID (bàn phím/chuột) — driver tự viết (P6.6, qua xHCI)";
    } else {
      r.path = Path::Basic;
      r.why = "USB HID trên máy không có xHCI: chỉ qua giả lập PS/2 của BIOS (P6.33)";
    }
    return r;
  }
  if (usb_has(d, 0x0e)) {
    r.cat = Category::Camera;
    r.path = Path::PassUsb;
    r.why = "camera UVC ⇒ qua micro-VM (P3.25); camera hồng ngoại kiểu Windows Hello: hạng D, chưa kiểm";
    return r;
  }
  if (usb_has(d, 0x08)) {
    r.cat = Category::MassStorage;
    r.path = Path::PassUsb;
    r.why = "ổ USB: P6.6 chỉ có HID ⇒ qua micro-VM (P3.25)";
    return r;
  }
  if (usb_has(d, 0x01)) {
    r.cat = Category::Audio;
    r.path = Path::PassUsb;
    r.why = "USB Audio Class: sau P6.6 ⇒ hiện qua micro-VM";
    return r;
  }
  if (usb_has(d, 0x02) || usb_has(d, 0x0a)) {
    r.cat = Category::Ethernet;
    r.path = Path::PassUsb;
    r.why = "USB-Ethernet/modem (lớp CDC): chưa có task driver gốc ⇒ qua micro-VM (P3.25)";
    return r;
  }
  if (usb_has(d, 0xe0)) {
    r.cat = Category::Bluetooth;
    r.path = Path::PassUsb;
    r.why = "dongle Bluetooth ⇒ qua net-vm (P6.46); sự kiện BT không tính là phím vật lý (P2.21)";
    return r;
  }
  if (usb_has(d, 0x0b)) {
    r.cat = Category::SmartCard;
    r.path = Path::PassUsb;
    r.why = "đầu đọc thẻ thông minh ⇒ qua micro-VM";
    return r;
  }
  if (usb_has(d, 0x07)) {
    r.cat = Category::Printer;
    r.path = Path::PassUsb;
    r.why = "máy in USB ⇒ qua micro-VM";
    return r;
  }
  if (usb_has(d, 0xff)) {
    r.cat = Category::VendorSpecific;
    r.path = Path::Unknown;
    r.why =
        "lớp riêng của hãng (vân tay, dongle Wi-Fi, phím riêng…): không suy ra được — có driver Linux "
        "mainline thì dự kiến B, chỉ có driver Windows thì D; cần đo";
    return r;
  }
  r.cat = Category::Other;
  r.path = Path::Unknown;
  r.why = "lớp USB chưa có quy tắc — cần đo";
  return r;
}

void apply_path(const Rule& rule, const Platform& p, Verdict& v) {
  v.category = rule.cat;
  v.expected_why = rule.why;
  v.guide_vi = rule.guide;
  const bool virt = p.vmx || p.svm;
  const char* iommu_name = p.svm && !p.vmx ? "AMD-Vi (IOMMU)" : "VT-d";
  switch (rule.path) {
    case Path::Native: v.expected = Grade::A; break;
    case Path::Basic: v.expected = Grade::C; break;
    case Path::Unsupported: v.expected = Grade::D; break;
    case Path::Infra:
    case Path::Unknown: v.expected = Grade::None; break;
    case Path::PassUsb:
    case Path::PassPci: {
      const bool pci = rule.path == Path::PassPci;
      if (!p.cpu_flags_known) {
        v.expected = Grade::B;
        v.expected_why += "; chưa đọc được cờ ảo hoá của CPU ⇒ B chỉ khi máy có VT-x/AMD-V" +
                          std::string(pci ? " + IOMMU" : "");
      } else if (!virt) {
        v.expected = Grade::D;
        v.expected_why += "; CPU không báo VT-x/AMD-V ⇒ không có micro-VM";
        v.guide_vi.push_back(
            "Bật VT-x/AMD-V (\"Virtualization Technology\") trong BIOS nếu CPU có; nếu không, thiết bị "
            "này chỉ dùng được ở Linux host (P1).");
      } else if (pci && p.iommu_table.empty()) {
        v.expected = Grade::D;
        v.expected_why += std::string("; không có bảng ACPI DMAR/IVRS ⇒ không cấp thẻ PCIe an toàn (tấn công DMA)");
        v.guide_vi.push_back(std::string("Bật ") + iommu_name +
                             " trong BIOS rồi quét lại: thẻ PCIe chỉ được cấp cho micro-VM khi có IOMMU.");
      } else {
        v.expected = Grade::B;
        if ((p.vmx && !p.ept) || (p.svm && !p.npt))
          v.expected_why += "; CPU không có EPT/NPT ⇒ micro-VM chậm";
      }
      break;
    }
  }
}

std::string grade_from_text(const std::string& s, Grade& g) {
  if (s == "A") g = Grade::A;
  else if (s == "B") g = Grade::B;
  else if (s == "C") g = Grade::C;
  else if (s == "D") g = Grade::D;
  else if (s == "-") g = Grade::None;
  else return "hạng không hợp lệ: " + s;
  return "";
}

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t t = line.find('\t', start);
    out.push_back(line.substr(start, t == std::string::npos ? std::string::npos : t - start));
    if (t == std::string::npos) break;
    start = t + 1;
  }
  return out;
}

bool is_x86_flags_line(const std::string& key) { return key == "flags"; }

void read_cpu(const std::string& root, Platform& p) {
  const std::string text = read_small(root + "/proc/cpuinfo", 1 << 16);
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = yui::text::trim(line.substr(0, colon));
    const std::string val = yui::text::trim(line.substr(colon + 1));
    if (key == "model name" && p.cpu_model.empty()) p.cpu_model = val;
    const bool flags = is_x86_flags_line(key), vmxf = key == "vmx flags";
    if (!flags && !vmxf) continue;
    if (flags) {
      if (p.cpu_flags_known) continue;  // chỉ CPU đầu tiên
      p.cpu_flags_known = true;
    }
    std::istringstream fs(val);
    std::string f;
    while (fs >> f) {
      if (flags && f == "vmx") p.vmx = true;
      else if (flags && f == "svm") p.svm = true;
      else if (flags && f == "npt") p.npt = true;
      else if (flags && f == "hypervisor") p.hypervisor = true;
      else if (f == "ept") p.ept = true;  // cũ: trong "flags"; nhân mới: trong "vmx flags"
    }
  }
}

void read_disks(const std::string& root, Platform& p) {
  const std::string dir = root + "/sys/block";
  for (const auto& n : list_dir(dir)) {
    if (n.rfind("loop", 0) == 0 || n.rfind("ram", 0) == 0 || n.rfind("zram", 0) == 0 ||
        n.rfind("dm-", 0) == 0 || n.rfind("md", 0) == 0 || n.rfind("sr", 0) == 0 ||
        n.rfind("nbd", 0) == 0)
      continue;
    Disk d;
    d.name = n;
    const std::string base = dir + "/" + n;
    const std::string target = link_target(base);
    if (n.rfind("nvme", 0) == 0) d.kind = "NVMe";
    else if (n.rfind("mmcblk", 0) == 0) d.kind = "SD/eMMC";
    else if (n.rfind("vd", 0) == 0) d.kind = "VirtIO";
    else if (target.find("/usb") != std::string::npos) d.kind = "USB";
    else if (n.rfind("sd", 0) == 0 || n.rfind("hd", 0) == 0) d.kind = "SATA/SCSI";
    else d.kind = "khác";
    d.rotational = read_small(base + "/queue/rotational", 16) == "1";
    d.removable = read_small(base + "/removable", 16) == "1";
    const std::string sz = read_small(base + "/size", 32);
    d.size_bytes = sz.empty() ? 0 : uint64_t(std::strtoull(sz.c_str(), nullptr, 10)) * 512u;
    p.disks.push_back(d);
  }
}

std::vector<Device> read_pci(const std::string& root) {
  std::vector<Device> out;
  const std::string dir = root + "/sys/bus/pci/devices";
  for (const auto& slot : list_dir(dir)) {
    const std::string b = dir + "/" + slot;
    uint32_t v = 0, dv = 0, cls = 0;
    if (!parse_hex(read_small(b + "/vendor", 32), 0xffff, v)) continue;
    if (!parse_hex(read_small(b + "/device", 32), 0xffff, dv)) continue;
    if (!parse_hex(read_small(b + "/class", 32), 0xffffff, cls)) cls = 0;
    Device d;
    d.bus = Bus::Pci;
    d.vendor = uint16_t(v);
    d.device = uint16_t(dv);
    d.pci_class = cls;
    d.slot = slot;
    d.linux_driver = basename_of(link_target(b + "/driver"));
    out.push_back(d);
  }
  return out;
}

std::vector<Device> read_usb(const std::string& root) {
  std::vector<Device> out;
  const std::string dir = root + "/sys/bus/usb/devices";
  const auto names = list_dir(dir);
  for (const auto& n : names) {
    if (n.find(':') != std::string::npos) continue;  // interface, gộp vào thiết bị cha
    if (n.rfind("usb", 0) == 0) continue;            // root hub ảo của bộ điều khiển
    const std::string b = dir + "/" + n;
    uint32_t v = 0, pr = 0, cls = 0;
    if (!parse_hex(read_small(b + "/idVendor", 32), 0xffff, v)) continue;
    if (!parse_hex(read_small(b + "/idProduct", 32), 0xffff, pr)) continue;
    Device d;
    d.bus = Bus::Usb;
    d.vendor = uint16_t(v);
    d.device = uint16_t(pr);
    d.slot = n;
    d.name = read_small(b + "/product", 256);
    if (parse_hex(read_small(b + "/bDeviceClass", 16), 0xff, cls) && cls != 0x00 && cls != 0xef)
      d.usb_classes.push_back(uint8_t(cls));
    const std::string prefix = n + ":";
    for (const auto& i : names) {
      if (i.rfind(prefix, 0) != 0) continue;
      uint32_t ic = 0;
      if (parse_hex(read_small(dir + "/" + i + "/bInterfaceClass", 16), 0xff, ic) &&
          !usb_has(d, uint8_t(ic)))
        d.usb_classes.push_back(uint8_t(ic));
      if (d.linux_driver.empty()) d.linux_driver = basename_of(link_target(dir + "/" + i + "/driver"));
    }
    out.push_back(d);
  }
  return out;
}

bool device_less(const Device& a, const Device& b) {
  if (a.bus != b.bus) return a.bus == Bus::Pci;
  if (a.slot != b.slot) return a.slot < b.slot;
  if (a.vendor != b.vendor) return a.vendor < b.vendor;
  return a.device < b.device;
}

void add_unique(std::vector<std::string>& v, const std::string& s) {
  if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

}  // namespace

const char* grade_code(Grade g) {
  switch (g) {
    case Grade::A: return "A";
    case Grade::B: return "B";
    case Grade::C: return "C";
    case Grade::D: return "D";
    case Grade::None: break;
  }
  return "-";
}

const char* category_vi(Category c) {
  switch (c) {
    case Category::Display: return "hiển thị";
    case Category::Storage: return "lưu trữ";
    case Category::UsbController: return "bộ điều khiển USB";
    case Category::Audio: return "âm thanh";
    case Category::Ethernet: return "mạng dây";
    case Category::Wireless: return "Wi-Fi";
    case Category::Bluetooth: return "Bluetooth";
    case Category::Input: return "bàn phím/chuột";
    case Category::Camera: return "camera";
    case Category::MassStorage: return "ổ USB";
    case Category::SmartCard: return "thẻ thông minh";
    case Category::Printer: return "máy in";
    case Category::VendorSpecific: return "riêng của hãng";
    case Category::Infra: return "hạ tầng";
    case Category::Other: break;
  }
  return "khác";
}

const HwdbEntry* Hwdb::find(Bus bus, uint16_t vendor, uint16_t device) const {
  for (const auto& r : rows)
    if (r.bus == bus && r.vendor == vendor && r.device == device) return &r;
  return nullptr;
}

Hwdb parse_hwdb(const std::string& text) {
  Hwdb db;
  std::istringstream in(text);
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (yui::text::trim(line).empty() || line[0] == '#') continue;
    const auto f = split_tabs(line);
    const std::string where = "dòng " + std::to_string(lineno) + ": ";
    if (f.size() != 6) {
      db.errors.push_back(where + "cần đúng 6 cột cách nhau bằng TAB, có " + std::to_string(f.size()));
      continue;
    }
    HwdbEntry e;
    if (f[0] == "pci") e.bus = Bus::Pci;
    else if (f[0] == "usb") e.bus = Bus::Usb;
    else {
      db.errors.push_back(where + "bus phải là pci hoặc usb");
      continue;
    }
    uint32_t v = 0, d = 0;
    const size_t colon = f[1].find(':');
    if (f[1].size() != 9 || colon != 4 || !parse_hex(f[1].substr(0, 4), 0xffff, v) ||
        !parse_hex(f[1].substr(5), 0xffff, d)) {
      db.errors.push_back(where + "mã phải có dạng vvvv:dddd (hex thường)");
      continue;
    }
    e.vendor = uint16_t(v);
    e.device = uint16_t(d);
    const std::string gerr = grade_from_text(f[2], e.grade);
    if (!gerr.empty()) {
      db.errors.push_back(where + gerr);
      continue;
    }
    e.source = f[3];
    e.name = f[4];
    e.note = f[5];
    if (e.source != "real" && e.source != "qemu" && e.source != "chưa-đo") {
      db.errors.push_back(where + "nguồn phải là real, qemu hoặc chưa-đo");
      continue;
    }
    if (e.source == "chưa-đo" && e.grade != Grade::None) {
      db.errors.push_back(where + "thiết bị chưa đo không được điền hạng (P6.31)");
      continue;
    }
    if (e.source != "chưa-đo" && e.grade == Grade::None) {
      db.errors.push_back(where + "nguồn real/qemu phải có hạng A–D");
      continue;
    }
    if (e.source != "chưa-đo" && yui::text::trim(e.note).empty()) {
      db.errors.push_back(where + "hạng đã đo phải có bằng chứng ở cột ghi chú");
      continue;
    }
    if (db.find(e.bus, e.vendor, e.device)) {
      db.errors.push_back(where + "trùng mã " + f[0] + " " + f[1]);
      continue;
    }
    db.rows.push_back(e);
  }
  return db;
}

Hwdb load_default_hwdb(std::string* used) {
  const char* env = std::getenv("YUI_HWDB");
  const std::string candidates[] = {env ? env : "", "docs/hwdb.tsv", "/usr/local/share/yui/hwdb.tsv",
                                    "/usr/share/yui/hwdb.tsv"};
  for (const auto& c : candidates) {
    if (c.empty()) continue;
    std::ifstream f(c, std::ios::binary);
    if (!f) continue;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (used) *used = c;
    return parse_hwdb(ss.str());
  }
  if (used) used->clear();
  return Hwdb{};
}

Verdict classify(const Device& d, const Platform& p, const Hwdb& db) {
  Verdict v;
  apply_path(d.bus == Bus::Pci ? pci_rule(d) : usb_rule(d, p.has_xhci), p, v);
  if (const HwdbEntry* e = db.find(d.bus, d.vendor, d.device)) {
    v.known_name = e->name;
    v.evidence = e->note;  // chưa đo: chỉ là ghi chú tham khảo, không thành hạng
    if (e->source == "real" || e->source == "qemu") {
      v.grade = e->grade;
      v.source = e->source;
    }
  }
  return v;
}

Report build(const Platform& p, std::vector<Device> devices, const Hwdb& db) {
  Report r;
  r.platform = p;
  std::sort(r.platform.disks.begin(), r.platform.disks.end(),
            [](const Disk& a, const Disk& b) { return a.name < b.name; });
  std::sort(devices.begin(), devices.end(), device_less);
  for (const auto& d : devices)
    if (d.bus == Bus::Pci && (d.pci_class & 0xffffff) == 0x0c0330) r.platform.has_xhci = true;

  // Hướng dẫn cấp máy trước, rồi hướng dẫn từng thiết bị (không trùng).
  if (p.hypervisor)
    add_unique(r.guide_vi,
               "Đang chạy trong máy ảo: bảng này mô tả phần cứng ảo, không phải máy thật — chạy lại trên máy thật "
               "để có số liệu `real`.");
  if (p.cpu_flags_known && !p.vmx && !p.svm)
    add_unique(r.guide_vi, "CPU không báo VT-x/AMD-V: bật \"Virtualization Technology\" trong BIOS nếu có; không "
                           "có thì mọi thiết bị hạng B rơi xuống D.");
  if (p.cpu_flags_known && (p.vmx || p.svm) && p.iommu_table.empty())
    add_unique(r.guide_vi, std::string("Không thấy bảng ACPI ") + (p.svm && !p.vmx ? "IVRS" : "DMAR") + ": bật " +
                               (p.svm && !p.vmx ? "AMD-Vi (IOMMU)" : "VT-d") +
                               " trong BIOS nếu có — thiếu IOMMU thì không cấp thẻ PCIe cho micro-VM.");
  if (p.cpu_flags_known && ((p.vmx && !p.ept) || (p.svm && !p.npt)))
    add_unique(r.guide_vi, "CPU có ảo hoá nhưng không có EPT/NPT ⇒ micro-VM (hạng B) sẽ chậm; ưu tiên thiết bị hạng "
                           "A/C.");
  if (!p.uefi)
    add_unique(r.guide_vi, "Máy khởi động kiểu BIOS (không thấy /sys/firmware/efi) ⇒ hiển thị qua VBE (P2.16), "
                           "cài đặt cần kịch bản BIOS (P4.20).");

  size_t unmeasured = 0;
  for (auto& d : devices) {
    Item it;
    it.device = d;
    it.verdict = classify(d, r.platform, db);
    if (it.verdict.category != Category::Infra && it.verdict.grade == Grade::None) ++unmeasured;
    for (const auto& g : it.verdict.guide_vi) add_unique(r.guide_vi, g);
    r.items.push_back(std::move(it));
  }
  if (unmeasured)
    add_unique(r.guide_vi, std::to_string(unmeasured) +
                               " thiết bị chưa đo: sau khi chạy thử, bạn có thể tự gửi PR vào docs/hwdb.tsv "
                               "(yui-hw không gửi gì đi đâu).");
  r.hwdb_errors = db.errors;
  return r;
}

Report scan(const std::string& root0, const Hwdb& db) {
  std::string root = root0;
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  if (root == "/") root.clear();
  Platform p;
  read_cpu(root, p);
  if (exists(root + "/sys/firmware/acpi/tables/DMAR")) p.iommu_table = "DMAR";
  else if (exists(root + "/sys/firmware/acpi/tables/IVRS")) p.iommu_table = "IVRS";
  p.iommu_active = !list_dir(root + "/sys/class/iommu").empty();
  p.uefi = exists(root + "/sys/firmware/efi");
  read_disks(root, p);
  std::vector<Device> devs = read_pci(root);
  for (auto& u : read_usb(root)) devs.push_back(std::move(u));
  return build(p, std::move(devs), db);
}

json::Value to_json(const Report& r) {
  json::Value root;
  root.set("schema", "yui-hw/1");
  root.set("note", "Hạng 'expected' là dự kiến theo quy tắc §14.3b; 'grade' chỉ có khi đã đo (real/qemu). "
                   "Không có % phần cứng được hỗ trợ.");
  json::Value plat;
  const Platform& p = r.platform;
  plat.set("cpu_model", p.cpu_model);
  plat.set("cpu_flags_known", p.cpu_flags_known);
  plat.set("vmx", p.vmx);
  plat.set("svm", p.svm);
  plat.set("ept", p.ept);
  plat.set("npt", p.npt);
  plat.set("hypervisor", p.hypervisor);
  plat.set("iommu_table", p.iommu_table);
  plat.set("iommu_active", p.iommu_active);
  plat.set("firmware", p.uefi ? "UEFI" : "BIOS");
  json::Array disks;
  for (const auto& d : p.disks) {
    json::Value o;
    o.set("name", d.name);
    o.set("kind", d.kind);
    o.set("rotational", d.rotational);
    o.set("removable", d.removable);
    o.set("size_bytes", static_cast<long long>(d.size_bytes));
    disks.push_back(o);
  }
  plat.set("disks", json::Value(disks));
  root.set("platform", plat);
  json::Array devs;
  for (const auto& it : r.items) {
    const Device& d = it.device;
    const Verdict& v = it.verdict;
    json::Value o;
    o.set("bus", bus_name(d.bus));
    o.set("slot", d.slot);
    o.set("id", hex4(d.vendor) + ":" + hex4(d.device));
    if (d.bus == Bus::Pci) {
      o.set("class", hex6(d.pci_class));
    } else {
      json::Array cls;
      for (uint8_t c : d.usb_classes) cls.push_back(hex2(c));
      o.set("classes", json::Value(cls));
    }
    o.set("name", !v.known_name.empty() ? v.known_name : d.name);
    o.set("linux_driver", d.linux_driver);
    o.set("category", category_vi(v.category));
    o.set("expected", grade_code(v.expected));
    o.set("expected_why", v.expected_why);
    o.set("grade", grade_code(v.grade));
    o.set("source", v.source);
    o.set("evidence", v.evidence);
    devs.push_back(o);
  }
  root.set("devices", json::Value(devs));
  json::Array guide;
  for (const auto& g : r.guide_vi) guide.push_back(g);
  root.set("guide", json::Value(guide));
  json::Array errs;
  for (const auto& e : r.hwdb_errors) errs.push_back(e);
  root.set("hwdb_errors", json::Value(errs));
  return root;
}

std::string summary_vi(const Report& r) {
  const Platform& p = r.platform;
  std::string s = "YuiVieOS · Trợ lý phần cứng (yui-hw)\n";
  s += "  CPU      : " + (p.cpu_model.empty() ? std::string("(không rõ)") : p.cpu_model) + "\n";
  s += "  Ảo hoá   : ";
  if (!p.cpu_flags_known) s += "chưa đọc được cờ CPU";
  else {
    s += p.vmx ? "VT-x" : p.svm ? "AMD-V" : "không có";
    if (p.vmx || p.svm) s += (p.ept || p.npt) ? " + EPT/NPT" : " (không EPT/NPT)";
  }
  if (p.hypervisor) s += " — đang trong máy ảo";
  s += "\n  IOMMU    : " + (p.iommu_table.empty() ? std::string("không thấy bảng DMAR/IVRS") : p.iommu_table) +
       (p.iommu_active ? " (nhân Linux đang bật)" : "") + "\n";
  s += std::string("  Firmware : ") + (p.uefi ? "UEFI" : "BIOS") + "\n";
  for (const auto& d : p.disks) {
    char sz[32];
    std::snprintf(sz, sizeof sz, "%.1f GB", double(d.size_bytes) / 1e9);
    s += "  Ổ đĩa    : " + d.name + " (" + d.kind + ", " + sz + (d.rotational ? ", đĩa quay" : "") +
         (d.removable ? ", tháo rời" : "") + ")\n";
  }
  if (r.items.empty()) {
    s += "\n  Không thấy thiết bị PCI/USB nào trong sysfs (container/máy ảo tối giản?) — chạy trên máy thật.\n";
  } else {
    s += "\n  Thiết bị (dự kiến = theo quy tắc; đã đo = chỉ khi có bằng chứng real/qemu):\n";
  }
  for (const auto& it : r.items) {
    const Device& d = it.device;
    const Verdict& v = it.verdict;
    if (v.category == Category::Infra) continue;
    std::string name = !v.known_name.empty() ? v.known_name : d.name;
    s += std::string("  ") + bus_name(d.bus) + " " + hex4(d.vendor) + ":" + hex4(d.device) + "  " +
         category_vi(v.category) + (name.empty() ? "" : " — " + name) + "\n";
    s += std::string("      dự kiến ") + grade_code(v.expected) + " · đã đo " + grade_code(v.grade) + " (" + v.source +
         ")\n      " + v.expected_why + "\n";
  }
  size_t infra = 0;
  for (const auto& it : r.items)
    if (it.verdict.category == Category::Infra) ++infra;
  if (infra) s += "  (+ " + std::to_string(infra) + " thiết bị hạ tầng chipset/hub, không cần hạng)\n";
  if (!r.guide_vi.empty()) {
    s += "\n  Hướng dẫn:\n";
    for (const auto& g : r.guide_vi) s += "  • " + g + "\n";
  }
  for (const auto& e : r.hwdb_errors) s += "  ! hwdb " + e + "\n";
  return s;
}

}  // namespace yui::hw
