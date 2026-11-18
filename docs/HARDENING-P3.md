# YuiVieOS Beta — Rà soát Ngữ cảnh và Mô hình Đe doạ Hộp cát VMM (HARDENING-P3.md)

Tài liệu này thực hiện nghiệm thu task **P3.7** theo `docs/PLAN.md` §5 và §16.
Mục tiêu: Rà soát toàn diện ngữ cảnh, ranh giới tin cậy (trust boundaries), hợp đồng năng lực
(capability contracts), baseline mã nguồn và danh mục rủi ro trước khi triển khai 3 lớp hardening
(P3.8, P3.9, P3.10, P3.11).

Ngày rà soát: **2026-11-18**.
Nhánh Git hiện hành: `arena/846d9f75-yuivieosbeta`.

---

## 1. Kiểm tra Baseline mã nguồn và Git Status

Đối chiếu trạng thái cây mã nguồn tại thời điểm rà soát:
- **Tài liệu nền tảng:** Đã đối chiếu `docs/PLAN.md`, `docs/ARCHITECTURE.md`, `docs/SECURITY.md`, `docs/PROTOCOL.md`, `docs/FORMAT-YUIP.md`, `README.md`.
- **Mã nguồn hộp cát & năng lực:** `vmm-sandbox/sandbox.cpp`, `include/yui/sandbox.hpp`, `kernel-cap/src/lib.rs`, `kernel-services/guard.cpp`, `ai-bridge-yuid/daemon/service.cpp`.
- **Bộ kiểm thử Red-Team & Hộp cát:** `tests/test_sandbox.cpp`, `tests/test_redteam.cpp`.

---

## 2. Ranh giới tin cậy (Trust Boundaries)

| Ranh giới | Bên tin cậy (Trusted) | Bên không tin cậy (Untrusted) | Cơ chế kiểm soát hiện tại | Cơ chế mở rộng ở P3.8–P3.11 |
|---|---|---|---|---|
| **R1: Tiến trình Host ↔ Guest Hộp cát** | Nhân YuiVieOS / Daemon `yuid` / `Enforcer` | Ứng dụng trong hộp cát `isolated`, micro-VM KVM, Wine, Waydroid | Cgroup, namespaces, policy JSON (`first-party-ipc`, `legacy-game`, `trim`, `yui-app`) | Lớp 2: Micro-VM quarantine, freeze rồi huỷ phiên nguyên tử (P3.9) |
| **R2: Mạng & Dữ liệu Egress** | `EgressFilter`, `Daemon` | Toàn bộ socket ra ngoài từ ứng dụng khách | Mặc định chặn (deny-by-default); chỉ cho phép host/CIDR/cổng trong allowlist; deny thắng allow | Lớp 3: Anti-DNS-tunneling (entropy > 4.2 bit/char, nhãn > 63 octet, tên > 253 octet), secretless broker (P3.10) |
| **R3: Bàn phím & Dữ liệu Nhập** | OS compositor, `ThirdPartyImeHost` | Engine bộ gõ bên thứ ba (IBus/Fcitx5) | Các trường nhạy cảm (`Password`, `Pin`, `DigitsOnly`, `Url`, `Email`) engine nhận đúng **0 phím** (P3.13) | Cách ly bộ nhớ và IPC chuyên dụng cho IME (P3.13) |
| **R4: Điều khiển Hệ điều hành (OS MCP)** | `Daemon::handle`, `Authority` | Tác tử AI / Client MCP bên ngoài | Token HMAC SHA-256 fail-closed, bắt buộc `confirm: true` không side-effect, che giấu bí mật trong audit (P4.3, P7.5) | Lớp 1: Thu hồi bắc cầu cả họ token với độ phức tạp **O(1)** theo generation/epoch (P3.8) |

---

## 3. Hợp đồng Năng lực (Capability Contracts)

1. **Khớp mã nguồn Rust `kernel-cap`:**
   - Cấu trúc Token: `subject`, `rights` (bitmask 16 quyền), `scope` (danh sách tiền tố đường dẫn/tài nguyên), `issued_at`, `expires_at`, `generation`, `signature` (HMAC-SHA256).
   - Nguyên tắc: **Fail-Closed**. Thiếu token, sai chữ ký, hết hạn, hoặc quyền không bao phủ hành động ⇒ từ chối lập tức và ghi nhận vào `audit.log`.
2. **Quy tắc thu hồi O(1):**
   - Thu hồi theo `generation` hoặc `epoch`: Khi một token bị thu hồi, toàn bộ token con (descendants) được dẫn xuất từ nó tự động bị vô hiệu hoá tại lần xác minh kế tiếp vì `token.generation < current_epoch(subject)`.
   - Không duyệt cây, không rò rỉ trạng thái giữa các subject khác nhau.

---

## 4. Các rủi ro đã kiểm chứng và Kết quả Red-Team

1. **P3.5 — 10/10 kịch bản cố thoát hộp cát thất bại:**
   - Đọc `/etc/shadow`, mở socket C2, gọi `ptrace`, `mount`, truy cập `/proc/self/mem`, leo thang quyền `admin`, gửi input injection, nạp `kernel.module`, can thiệp `/dev/mem`, mở cổng lắng nghe `net.listen`.
   - Toàn bộ 10/10 kịch bản bị chặn với mã lý do tiếng Việt rõ ràng.
2. **P3.12 — Cổng chữ ký số ứng dụng:**
   - Ứng dụng không ký hoặc ký khoá lạ ⇒ giam giữ trong hộp cát `isolated`, tước toàn bộ quyền mạng và quản trị.
   - Gói bị can thiệp dù 1 byte ⇒ từ chối cài đặt.
3. **P3.13 — Bảo vệ trường nhạy cảm trước bộ gõ ngoài:**
   - Không có bất kỳ phím mật khẩu/PIN nào lọt tới engine IBus/Fcitx5 bên thứ ba.
   - Engine độc hại cố mở mạng hoặc đọc trộm tệp SSH key bị chặn 100%.

---

## 5. Quyết định Mở và Lộ trình thi công P3.8–P3.11

1. **Thứ tự thực hiện bắt buộc (§5.1):**
   - **P3.7 (Rà soát ngữ cảnh & Threat Model):** Hoàn tất (tài liệu này).
   - **P3.8 (Lớp 1 - Capability Boundary & O(1) Transitive Revocation):** Bổ sung generation counter và logic xác minh epoch trong `vmm-sandbox/sandbox.cpp` và `tests/test_redteam.cpp`.
   - **P3.9 (Lớp 2 - Micro-VM Quarantine & Freeze Lifecycle):** Thiết kế máy trạng thái `running → frozen → destroyed` cho micro-VM.
   - **P3.10 (Lớp 3 - DNS Tunneling Detection & Secretless Broker):** Tính toán Shannon entropy cho truy vấn DNS, bộ lọc nhãn và cơ chế gắn token ở vòng ngoài.
   - **P3.11 (Red-Team tích hợp ba lớp):** Kiểm chứng toàn bộ chuỗi phản ứng liên hoàn.
2. **Lưu ý đồng bộ với P4.3:**
   - Việc P4.3 đã hoàn tất nghiệm thu ở Tầng 4 (OS MCP control) không làm ảnh hưởng tính độc lập của việc hardening Tầng 3; tài liệu rà soát này chuẩn hoá ranh giới giữa hai tầng.
