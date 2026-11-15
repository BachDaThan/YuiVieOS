# kernel-services — Tầng 2: Nhân Dịch Vụ Hiệu Năng Cao (Safe Rust Framekernel)

Chính sách của nhân, viết bằng Rust an toàn. Cơ chế (chuyển ngữ cảnh, MMU, ngắt)
nằm ở khung phần cứng duy nhất — xem `docs/PLAN.md` P2.1.

| Thành phần | Việc | Ngôn ngữ |
|---|---|---|
| `yui-sched/` | Lập lịch chuyển đổi nóng LAVD: P/E-core, tín dụng đánh thức, vruntime theo trọng số, công bằng | Rust |
| `yui-zram/` | ZRAM: codec YUI-LZ + kho trang nén + thống kê tiết kiệm | Rust |
| `codec.cpp` | Bản tham chiếu YUI-LZ/container YUIP dùng cho `.yuip` (đối xứng byte với crate) | C++17 |
| `crypto.cpp` | SHA-256/HMAC/SHAKE/scrypt của nhân | C++17 |
| `profile.cpp` | Hardware Auto-Profiler: CPU/RAM/GPU/pin/nhiệt → `may-yeu` / `can-bang` / `may-manh` + k/W | C++17 |

## Vì sao có cả Rust và C++ ở đây

`yui-sched` và `yui-zram` là *chính sách* — phần dễ sai nhất và cần hot-swap,
nên viết bằng Rust an toàn và test được không cần boot. `codec.cpp`/`profile.cpp`
là phần đã được kiểm chứng trong bộ test C++ (705 check) và đang được dùng bởi
`yuid`, `yui-trim`; chúng sẽ chuyển sang `yui-frame` ở P2.3 khi khung Rust chạy
được. Không có hai bản cài đặt nào *cùng* là nguồn sự thật: Rust là đích, C++ là
bản chạy được hôm nay (xem bảng "Trạng thái" trong README).

## Ngân sách tài nguyên (tiêu chí P1)

| Chỉ số | Mục tiêu máy yếu (RAM 512 MB–2 GB) |
|---|---|
| RAM sau khởi động (nhân + dịch vụ bắt buộc) | < 64 MB |
| ZRAM tỉ lệ nén trên dữ liệu văn phòng | ≥ 2,5× |
| Độ trễ tickless (không có timer định kỳ khi rảnh) | 0 wakeup/s khi rảnh |
| Trượt lập lịch của tác vụ tương tác (p99) | < 5 ms |
