# YuiVieOS Beta — Thành phần bên thứ ba và Nghĩa vụ Giấy phép (THIRD-PARTY.md)

Tài liệu này quản lý danh mục các thành phần phần mềm bên thứ ba dự kiến được sử dụng
hoặc tích hợp trong hệ sinh thái YuiVieOS Beta, phục vụ việc rà soát pháp lý (Phase P7),
truy xuất nguồn gốc thành phần (P4.11 SBOM) và đảm bảo tuân thủ đầy đủ các giấy phép
nguồn mở.

Ngày rà soát gần nhất: **2026-11-18**. Danh mục bổ sung (§5–§6) thêm **2026-11-19**: dòng ghi *chưa kiểm* nghĩa là mình **chưa đọc giấy phép gốc** và chưa được đưa vào bản build nào.

---

## 1. Tuyên bố kiến trúc lõi: Không dependency ngoài

Hiện tại (tại thời điểm commit này, 2026-11-18):
- **Tầng 1 (Cơ chế Năng lực - `kernel-cap`):** 100% Rust `no_std`, `deny(unsafe_code)`, **0 dependency ngoài** (chỉ dùng `core`).
- **Tầng 2 (Dịch vụ Nhân - `kernel-services/*`):** 100% Rust `no_std`, `deny(unsafe_code)`, **0 dependency ngoài**.
- **Khung Phần cứng (`kernel-frame`):** 100% Rust `no_std`, chỉ phụ thuộc path nội bộ `kernel-cap`.
- **Dịch vụ C++ (`ai-bridge-yuid`, `userspace-vi`, `vmm-sandbox`, `pkg-stripper`, `tools`):** Chỉ dùng chuẩn C++17 / C11 và API hệ thống POSIX chuẩn của Linux host; không vendor thư viện ngoài.

Quy tắc bất biến: **`scripts/verify.py::check_kernel_dependencies` và `check_third_party` tự động kiểm tra và báo đỏ (FAIL) nếu phát hiện bất kỳ thư mục `vendor/*` hoặc dependency không-path nào chưa được khai báo.**

YuiVieOS Beta **tuyệt đối không đóng gói hay phân phối lại** các tệp nhị phân đóng nguồn độc quyền của Microsoft Windows (DLLs/runtime) hay các dịch vụ đóng nguồn của Google (Google Mobile Services - GMS), **bộ dịch mã đóng nguồn** (ví dụ libhoudini của Intel) và **bản vá/crack của game**.

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
4. **Không bao giờ nằm trong ISO hay manifest mặc định:** bộ dịch mã đóng nguồn, Google Mobile Services, bản vá game. Chỉ có cơ chế nạp; người dùng tự tải từ nguồn chính thức và tự theo giấy phép của họ (P3.20, P7.10).
3. **Chống Zip-Slip và kiểm tra chữ ký:** Mọi gói thành phần bên thứ ba tải về phải được thẩm định chữ ký số Ed25519 hoặc mã băm SHA-256 đối chiếu với kho khoá chủ quyền (`bundle::Keyring` của P2.4 / P3.12).

---

## 4. Kiểm toán tự động (CI / verify.sh)

Hệ thống CI và script `scripts/verify.py::check_third_party` kiểm tra:
1. Tệp `docs/THIRD-PARTY.md` tồn tại và đầy đủ thông tin các thành phần dự kiến.
2. Thư mục `vendor/` không tồn tại trong kho (trừ khi có sự phê duyệt rõ ràng và được ghi nhận trong bảng trên).
3. Các file Cargo.toml Tầng 1–2 giữ vững nguyên tắc 0 dependency ngoài.

---

## 5. Thành phần dự kiến thêm (đề xuất 2026-11-19; giấy phép CHƯA kiểm trừ khi ghi khác)

| Thành phần | Dùng ở | Giấy phép (theo hiểu biết, chưa đọc bản gốc) | Cách phân phối | Việc cần làm trước khi dùng |
|---|---|---|---|---|
| Ảnh hệ thống Android của Waydroid | P3.4, P3.17 | Hỗn hợp (phần lớn mã nguồn mở; nhân Linux GPL-2.0) | Waydroid tải từ nguồn của nó khi bạn cài; không đóng gói | Đọc giấy phép ảnh, ghi ngày |
| libhoudini (Intel) | P3.20 | **Độc quyền, đóng nguồn** | **Không đóng gói**; bạn tự tải | Đọc điều khoản; không phân phối lại |
| libndk_translation | P3.20 | **Chưa kiểm** (Gemini xếp "đóng nguồn", mình chưa xác minh) | Như trên | Đọc giấy phép trước khi viết dòng nào |
| Google Mobile Services | — | **Độc quyền** | **Không đóng gói** | Không làm trong dự án |
| SDL_GameControllerDB | P3.22 | zlib (chưa kiểm) | Chỉ khi bạn duyệt | Đọc LICENSE của kho đó |
| WireGuard (module nhân Linux khách) | P3.18 bước 2 | GPL-2.0 | Chạy trong khách Linux, không liên kết | Duyệt dependency theo PLAN §6.1 mục 5 |
| Danh sách chặn tên miền (StevenBlack/hosts, OISD, …) | P4.18 | StevenBlack: MIT (chưa kiểm hôm nay); danh sách khác: chưa biết | Tải khi **bạn bật**; không đóng gói | Đọc từng danh sách (P7.11) |
| Ứng dụng trong Kho Ứng Dụng (trình duyệt, Zalo, …) | P4.15 | Mỗi ứng dụng một giấy phép/nhãn hiệu | Tải trực tiếp từ nguồn chính thức; **không mirror** | Ghi nguồn + giấy phép + ngày đọc vào manifest (P7.11) |
| Samba | Backlog NAS | GPL-3.0 | Tải theo nhu cầu trong micro-VM; không đóng gói | Đọc giấy phép khi lập task |

---

## 7. Bộ nạp, firmware, ảnh khởi động, driver ngoài (đề xuất 2026-11-19; giấy phép CHƯA kiểm trừ khi ghi khác — P7.14)

| Thành phần | Dùng ở | Giấy phép (theo hiểu biết, chưa đọc bản gốc hôm nay) | Cách phân phối | Việc cần làm trước khi dùng |
|---|---|---|---|---|
| Firmware/khối nhị phân của nhà sản xuất (Wi-Fi, GPU…) từ kho firmware của Linux | P3.26, P6.31 | **Mỗi khối một giấy phép; có khối chỉ cho phân phối dạng nhị phân** | **Không đóng gói trong ISO**; tải theo nhu cầu từ nguồn chính thức, kiểm băm | Đọc giấy phép từng khối, ghi ngày |
| GRUB 2 | P4.20, P4.21 | GPL-3.0-or-later | Chỉ khi **đóng gói** trong ISO ⇒ nghĩa vụ cung cấp mã nguồn | Quyết định có đóng gói hay không; nếu có, kèm mã nguồn tương ứng |
| Limine | P4.20, P4.21 | BSD-2-Clause | Đính kèm nếu dùng | Đọc LICENSE bản dùng |
| shim | P6.8 | BSD-2-Clause (theo hiểu biết); bản có chữ ký Microsoft do bên phân phối Linux cấp | Chưa dùng; bậc 3 ở PLAN §14.3c là backlog | Đọc điều kiện shim-review/ký trước khi bàn tới |
| xorriso | P4.21 | GPL-3.0-or-later | Công cụ dựng ISO, chạy ở bước build, không nằm trong ảnh | Không liên kết vào mã của dự án |
| Ventoy, Rufus, balenaEtcher | P4.21 | Mỗi công cụ một giấy phép | **Chỉ nhắc như công cụ của người dùng**, không đóng gói, không trỏ liên kết tải trong ảnh | — |
| Phông chữ lớn hơn cho tiếng Việt (ví dụ họ Noto, Be Vietnam Pro) | P2.16 | SIL OFL 1.1 (theo hiểu biết) | Chỉ khi bạn duyệt | Đọc giấy phép từng phông; OFL có điều kiện về tên khi sửa đổi |
| Bản phân phối Linux nền cho YuiVieOS Lite | P1.13 | **Chưa chọn**; gồm nhiều giấy phép (GPL, LGPL, …) và **điều khoản nhãn hiệu của bản gốc** | Chưa có | Đọc chính sách làm bản phái sinh/nhãn hiệu của bản được chọn; kèm đề nghị cung cấp mã nguồn |

## 8. Nguồn dữ liệu của Yui Store (đề xuất 2026-11-19 — P4.29, P7.17)

Nguyên tắc: Store chỉ hiển thị **metadata** và chuyển luồng tải **trực tiếp từ máy chủ gốc**; **không lưu lại** tệp cài đặt của bên thứ ba; có bộ nhớ đệm cục bộ và tôn trọng giới hạn lượt gọi; **không cào** kho đóng (Google Play, App Store). Ngày đọc điều khoản chỉ ghi khi **đã đọc thật**.

| Nguồn | Dùng ở | Điều khoản/giấy phép | Ngày đọc | Ghi chú |
|---|---|---|---|---|
| `catalog.json` do dự án ký (Ed25519) | P4.15, P4.29 (a) | Của dự án (MIT) | — | Chỉ chứa metadata + băm |
| F-Droid `index-v2.json` | P4.29 (b) | **Chưa đọc** | chưa | Chỉ mục công khai; ảnh/biểu tượng có thể có bản quyền riêng |
| Flathub (API/AppStream) | P4.29 (b) | **Chưa đọc** | chưa | Tôn trọng giới hạn lượt gọi |
| Winget (manifest) | P4.29 (b) | **Chưa đọc** | chưa | Gói Windows ⇒ chỉ có nghĩa qua lớp Wine |
| GitHub/GitLab Releases (API) | P4.29 (c) | **Chưa đọc** | chưa | Gọi không đăng nhập có giới hạn lượt thấp; có bộ nhớ đệm |
| Ảnh chụp màn hình/biểu tượng của bên thứ ba | P4.29 | Mỗi nguồn một quyền | — | Chỉ hiển thị khi nguồn cho phép; lưu bộ nhớ đệm cục bộ; không phân phối lại |
| Obtainium (tham chiếu ý tưởng) | P4.29 | Chưa kiểm | — | **Chỉ học ý tưởng UX**, không dùng mã |

## 9. Thành phần ảo hoá đồ họa, hệ tệp, container, kết nối (đề xuất 2026-11-19; giấy phép CHƯA kiểm trừ khi ghi khác)

| Thành phần | Dùng ở | Giấy phép (theo hiểu biết, chưa đọc bản gốc hôm nay) | Cách phân phối | Việc cần làm trước khi dùng |
|---|---|---|---|---|
| Mesa (Lavapipe/llvmpipe), LLVM | P3.29 | Mesa: chủ yếu MIT; LLVM: Apache-2.0 kèm ngoại lệ LLVM | Tải theo nhu cầu trong khách, không đóng gói | Đọc giấy phép từng phần |
| virglrenderer (VirGL/Venus) | P3.29 | MIT (theo hiểu biết) | Chỉ ở Linux host/bản Lite | Đọc LICENSE |
| virtiofsd / virtio-fs | P3.30 | Chưa kiểm (có bản Rust và bản C) | Daemon phía host: viết clean-room hoặc duyệt dependency (§6.1 mục 5) | Chọn và đọc giấy phép |
| runc / crun (runtime OCI) | P3.28 | runc: Apache-2.0 (theo hiểu biết); crun: **chưa kiểm** | Chạy **trong khách Linux**, không liên kết | Đọc giấy phép trước khi chọn |
| Wine 11 + NTSync (nhân Linux khách ≥ 6.14) | P3.27 | LGPL-2.1+ (Wine); GPL-2.0 (nhân Linux) | Như mục 2; nhân khách **không** nằm trong lõi YuiVieOS | Giữ ranh giới cách ly (mục 3) |
| scrcpy (tham chiếu cho `yui-droid-link`) | P6.41 | Apache-2.0 (theo hiểu biết) | **Chỉ học cách làm** (clean-room: không chép mã) | — |
| KDE Connect, Input Leap/Barrier/Synergy, WireGuard, OpenSSH | P6.40, P6.42, P6.45 | Bản cài đặt chủ yếu GPL/BSD; "Synergy", "WireGuard" là nhãn hiệu | **Chỉ tương thích giao thức**; không chép mã, không dùng tên làm tên sản phẩm | P7.18 |
| Bộ giải mã H.264/H.265, AAC/aptX/LDAC, RAR | P6.41, P6.46, P6.47, P4.33 | **Bằng sáng chế/giấy phép riêng hoặc định dạng đóng** | **Không đóng gói**; dùng codec của nền tảng hoặc codec mở (AV1, VP9, Opus) | P7.18 |

---

## 6. Công cụ tham chiếu — KHÔNG tích hợp, không đóng gói

| Công cụ | Vai trò | Ghi chú |
|---|---|---|
| mcpelauncher (minecraft-linux) | Mốc so sánh trên Linux host cho P6.27 | Giấy phép chưa kiểm; cách nó lấy APK qua đăng nhập Google Play chưa đọc điều khoản; **không** tích hợp vào OS |
| Phoronix Test Suite | Bộ đo so sánh (P6.26) | GPL-3.0; chạy như chương trình riêng, không đóng gói |
| lmbench / contextswitch | Đo vi mô (P6.26) | Giấy phép chưa kiểm |
| Minecraft (Mojang/Microsoft) | Game thử trong P6.27 | Độc quyền; bạn tự có bản mua hợp pháp |
| Bản vá Minecraft / MB Loader / shader | — | **Không bundle, không trỏ liên kết tải**; bạn tự chịu theo điều khoản của game |
| ApkStart (tham chiếu UX) | Ý tưởng "nhấp đúp APK ⇒ lối tắt" cho P4.24 | Chưa kiểm giấy phép; **chỉ học ý tưởng**, không dùng mã; không so sánh hơn thua |
