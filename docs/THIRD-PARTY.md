# YuiVieOS Beta — Thành phần bên thứ ba và Nghĩa vụ Giấy phép (THIRD-PARTY.md)

Tài liệu này quản lý danh mục các thành phần phần mềm bên thứ ba dự kiến được sử dụng
hoặc tích hợp trong hệ sinh thái YuiVieOS Beta, phục vụ việc rà soát pháp lý (Phase P7),
truy xuất nguồn gốc thành phần (P4.11 SBOM) và đảm bảo tuân thủ đầy đủ các giấy phép
nguồn mở.

Ngày rà soát gần nhất: **2026-11-18**.

---

## 1. Tuyên bố kiến trúc lõi: Không dependency ngoài

Hiện tại (tại thời điểm commit này, 2026-11-18):
- **Tầng 1 (Cơ chế Năng lực - `kernel-cap`):** 100% Rust `no_std`, `deny(unsafe_code)`, **0 dependency ngoài** (chỉ dùng `core`).
- **Tầng 2 (Dịch vụ Nhân - `kernel-services/*`):** 100% Rust `no_std`, `deny(unsafe_code)`, **0 dependency ngoài**.
- **Khung Phần cứng (`kernel-frame`):** 100% Rust `no_std`, chỉ phụ thuộc path nội bộ `kernel-cap`.
- **Dịch vụ C++ (`ai-bridge-yuid`, `userspace-vi`, `vmm-sandbox`, `pkg-stripper`, `tools`):** Chỉ dùng chuẩn C++17 / C11 và API hệ thống POSIX chuẩn của Linux host; không vendor thư viện ngoài.

Quy tắc bất biến: **`scripts/verify.py::check_kernel_dependencies` và `check_third_party` tự động kiểm tra và báo đỏ (FAIL) nếu phát hiện bất kỳ thư mục `vendor/*` hoặc dependency không-path nào chưa được khai báo.**

YuiVieOS Beta **tuyệt đối không đóng gói hay phân phối lại** các tệp nhị phân đóng nguồn độc quyền của Microsoft Windows (DLLs/runtime) hay các dịch vụ đóng nguồn của Google (Google Mobile Services - GMS).

---

## 2. Danh mục thành phần dự kiến tích hợp (Phase P3 & P6)

Khi người dùng kích hoạt các tính năng tương thích Windows, Android hoặc đo đạc đồ hoạ,
các thành phần sau sẽ được nạp trong hộp cát micro-VM cô lập hoặc tiến trình riêng:

| Thành phần | Mục đích trong YuiVieOS | Giấy phép | Cách phân phối | Nghĩa vụ chính | Ngày rà soát |
|---|---|---|---|---|---|
| **Wine** (Proton) | Lớp tương thích Windows API trong micro-VM (P3.2) | **LGPL-2.1-or-later** | Người dùng tải riêng / Tải tự động theo nhu cầu vào hộp cát (on-demand); **không** đóng gói trong ISO | Cung cấp mã nguồn tương ứng đã sửa đổi (nếu có), thông báo bản quyền LGPL, giữ nguyên tệp LICENSE/COPYING | 2026-11-18 |
| **VKD3D-Proton** | Lớp dịch Direct3D 12 sang Vulkan trong micro-VM (P3.2) | **LGPL-2.1-only** | Tải theo nhu cầu vào hộp cát đồ hoạ | Cung cấp mã nguồn tương ứng theo LGPL v2.1, thông báo bản quyền | 2026-11-18 |
| **DXVK** | Lớp dịch Direct3D 9/10/11 sang Vulkan (P3.2) | **Zlib License** | Tải theo nhu cầu vào hộp cát đồ hoạ | Giữ nguyên thông báo bản quyền Zlib trong tài liệu/bản phân phối | 2026-11-18 |
| **Waydroid** | Hộp cát LXC chạy ứng dụng Android không kèm Google GMS (P3.4) | **GPL-3.0-or-later** | Tải riêng / Kích hoạt theo nhu cầu vào micro-VM cô lập | Cung cấp toàn bộ mã nguồn tương ứng theo GPLv3, không áp dụng tivoization | 2026-11-18 |
| **Box64** | Lớp dịch nhị phân x86_64 sang ARM64/RISC-V 64 (P6.15) | **MIT License** | Module user-space độc lập trong hộp cát `isolated` | Đính kèm thông báo bản quyền và giấy phép MIT | 2026-11-18 |
| **FEX-Emu** | Lớp giả lập nhanh x86_64 sang AArch64 (P6.15) | **MIT License** | Module user-space độc lập trong hộp cát `isolated` | Đính kèm thông báo bản quyền và giấy phép MIT | 2026-11-18 |
| **MangoHud** | Lớp phủ HUD giám sát FPS, frame time, nhiệt độ, RAM trong game (P3.2, P6.11) | **MIT License** | Tiện ích tuỳ chọn trong hộp cát đồ hoạ | Đính kèm thông báo bản quyền và giấy phép MIT | 2026-11-18 |
| **Thư viện TLS** *(dự kiến mbedTLS hoặc rustls)* | Kết nối bảo mật MQTT/Egress cho IoT/cập nhật mạng (P6.20) | **Apache-2.0 / MIT** | Liên kết động hoặc tiến trình môi giới riêng | Giữ nguyên tệp NOTICE và giấy phép Apache-2.0 / MIT | 2026-11-18 |

---

## 3. Ranh giới cách ly giấy phép (License Isolation Boundary)

1. **Không lây nhiễm Copyleft:** Các thành phần mang giấy phép GPLv3 (như Waydroid) chỉ được thực thi như một khách micro-VM độc lập hoặc tiến trình cô lập phía trên ranh giới mạng/IPC tiêu chuẩn. Nhân YuiVieOS, thư viện C-ABI `libyui`, và daemon `yuid` **không liên kết tĩnh hay nhúng mã GPL** vào mã nguồn phân phối chung.
2. **Tuân thủ LGPL v2.1:** Các thành phần Wine và VKD3D-Proton nếu có sửa đổi phải được công bố mã nguồn phần sửa đổi theo đúng quy định của LGPL v2.1.
3. **Chống Zip-Slip và kiểm tra chữ ký:** Mọi gói thành phần bên thứ ba tải về phải được thẩm định chữ ký số Ed25519 hoặc mã băm SHA-256 đối chiếu với kho khoá chủ quyền (`bundle::Keyring` của P2.4 / P3.12).

---

## 4. Kiểm toán tự động (CI / verify.sh)

Hệ thống CI và script `scripts/verify.py::check_third_party` kiểm tra:
1. Tệp `docs/THIRD-PARTY.md` tồn tại và đầy đủ thông tin các thành phần dự kiến.
2. Thư mục `vendor/` không tồn tại trong kho (trừ khi có sự phê duyệt rõ ràng và được ghi nhận trong bảng trên).
3. Các file Cargo.toml Tầng 1–2 giữ vững nguyên tắc 0 dependency ngoài.
