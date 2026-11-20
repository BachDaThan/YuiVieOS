// yui-hw — Trợ lý phần cứng (P6.31): quét thiết bị, xếp hạng tương thích A/B/C/D
// theo §14.3b của docs/PLAN.md, in hướng dẫn tiếng Việt.
//
// Hai loại "hạng", KHÔNG được lẫn nhau:
//   * `expected` — hạng DỰ KIẾN do quy tắc suy ra từ lớp thiết bị + điều kiện
//     của máy (VT-x/AMD-V, IOMMU, EPT/NPT). Đây là suy luận, không phải số đo;
//     báo cáo luôn ghi "dự kiến".
//   * `grade` — hạng ĐÃ ĐO, chỉ lấy từ bảng `docs/hwdb.tsv` ở những dòng có
//     nguồn `real` (đã chạy trên máy thật) hoặc `qemu`, kèm bằng chứng. Thiết
//     bị chưa đo thì `grade = None` và nguồn "chưa đo" — luật của P6.31.
//
// Quét chỉ ĐỌC sysfs/procfs dưới một thư mục gốc (mặc định "/"; test dùng cây
// giả). Báo cáo chỉ ghi ra tệp cục bộ — không có mã mạng nào ở đây (P7.5).
// Không có "% phần cứng được hỗ trợ": chỉ có bảng từng thiết bị.
#ifndef YUI_HW_HPP
#define YUI_HW_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "yui/json.hpp"

namespace yui::hw {

enum class Bus { Pci, Usb };
enum class Grade { None, A, B, C, D };  // None = chưa đo / hạ tầng (không cần hạng)

const char* grade_code(Grade g);  // "-", "A", "B", "C", "D"

struct Device {
  Bus bus = Bus::Pci;
  uint16_t vendor = 0, device = 0;
  // PCI: class 24 bit (lớp << 16 | lớp con << 8 | prog-if), vd 0x030000 VGA.
  uint32_t pci_class = 0;
  // USB: lớp thiết bị (bDeviceClass) rồi lớp từng interface (bInterfaceClass).
  std::vector<uint8_t> usb_classes;
  std::string slot;          // "0000:00:02.0" hoặc "1-1.2"
  std::string name;          // tên sysfs nếu có (USB product), không bắt buộc
  std::string linux_driver;  // driver Linux đang giữ thiết bị (gợi ý, không dùng để xếp hạng)
};

struct Disk {
  std::string name;  // sda, nvme0n1, mmcblk0, vda…
  std::string kind;  // "NVMe", "SATA/SCSI", "USB", "SD/eMMC", "VirtIO", "khác"
  bool rotational = false, removable = false;
  uint64_t size_bytes = 0;
};

struct Platform {
  bool cpu_flags_known = false;  // đọc được dòng "flags" của /proc/cpuinfo (x86)
  bool vmx = false, svm = false, ept = false, npt = false;
  bool hypervisor = false;       // đang chạy TRONG máy ảo (cờ "hypervisor")
  std::string iommu_table;       // "DMAR" (Intel VT-d), "IVRS" (AMD-Vi) hoặc ""
  bool iommu_active = false;     // /sys/class/iommu có mục (nhân Linux đang bật IOMMU)
  bool uefi = false;             // /sys/firmware/efi tồn tại
  bool has_xhci = false;         // có bộ điều khiển PCI 0c0330 (build() tự điền từ danh sách)
  std::string cpu_model;
  std::vector<Disk> disks;
};

struct HwdbEntry {
  Bus bus = Bus::Pci;
  uint16_t vendor = 0, device = 0;
  Grade grade = Grade::None;
  std::string source;  // "real", "qemu", "chưa-đo"
  std::string name, note;
};

struct Hwdb {
  std::vector<HwdbEntry> rows;
  std::vector<std::string> errors;  // dòng hỏng/trái luật (bị bỏ, không dùng)
  const HwdbEntry* find(Bus bus, uint16_t vendor, uint16_t device) const;
};

/// TSV: bus<TAB>vvvv:dddd<TAB>hạng<TAB>nguồn<TAB>tên<TAB>ghi chú/bằng chứng.
/// Luật: hạng A–D chỉ khi nguồn là real/qemu VÀ có bằng chứng; nguồn chưa-đo
/// thì hạng phải là "-". Dòng trái luật bị bỏ và ghi vào `errors`.
Hwdb parse_hwdb(const std::string& text);
/// Nạp bảng mặc định: $YUI_HWDB, docs/hwdb.tsv, /usr/local/share/yui/hwdb.tsv,
/// /usr/share/yui/hwdb.tsv (tệp đầu tiên đọc được). `used` nhận đường dẫn
/// ("" nếu không thấy ⇒ bảng rỗng, mọi thiết bị "chưa đo").
Hwdb load_default_hwdb(std::string* used = nullptr);

enum class Category {
  Display, Storage, UsbController, Audio, Ethernet, Wireless, Bluetooth, Input,
  Camera, MassStorage, SmartCard, Printer, VendorSpecific, Infra, Other,
};
const char* category_vi(Category c);

struct Verdict {
  Category category = Category::Other;
  Grade expected = Grade::None;  // DỰ KIẾN theo quy tắc §14.3b
  std::string expected_why;      // lý do + task nối (tiếng Việt)
  Grade grade = Grade::None;     // ĐÃ ĐO (chỉ từ hwdb real/qemu)
  std::string source = "chưa đo";
  std::string evidence;          // bằng chứng của dòng hwdb (nếu có)
  std::string known_name;        // tên trong hwdb (nếu có)
  std::vector<std::string> guide_vi;
};

Verdict classify(const Device& d, const Platform& p, const Hwdb& db);

struct Item {
  Device device;
  Verdict verdict;
};

struct Report {
  Platform platform;
  std::vector<Item> items;           // đã sắp xếp (bus, slot) ⇒ JSON ổn định
  std::vector<std::string> guide_vi; // hướng dẫn gộp, không trùng
  std::vector<std::string> hwdb_errors;
};

/// Quét một cây sysfs/procfs có gốc `root` ("/" trên máy đang chạy).
Report scan(const std::string& root, const Hwdb& db);
/// Xếp hạng một danh sách thiết bị cho trước (test, hoặc dữ liệu nhập tay).
Report build(const Platform& p, std::vector<Device> devices, const Hwdb& db);

json::Value to_json(const Report& r);  // không có dấu thời gian ⇒ hai lần quét giống hệt nhau
std::string summary_vi(const Report& r);

}  // namespace yui::hw

#endif  // YUI_HW_HPP
