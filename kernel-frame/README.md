# `kernel-frame/` — khung phần cứng (Tầng 0)

Đây là **crate duy nhất trong toàn dự án được phép chứa `unsafe`**. Mọi crate
khác (`kernel-cap`, `kernel-services/*`, `vmm-sandbox`, `pkg-stripper`) giữ
`#![deny(unsafe_code)]` và `scripts/verify.sh` sẽ đỏ nếu bạn thêm `unsafe` vào đó.

## Vì sao phải có `unsafe`

Bốn việc không thể làm bằng Rust an toàn:

1. **chuyển ngữ cảnh** — x86_64 khởi động ở 32-bit; phải dựng bảng trang, bật
   PAE/LME/PG rồi nhảy sang mã 64-bit (`src/arch/x86_64.rs`);
2. **ngõ vào/ra** — cổng COM1 (`in`/`out`), UART PL011 (ghi MMIO), SBI `ecall`;
3. **bộ cấp phát** — triển khai `GlobalAlloc` trên một vùng tĩnh
   (`src/allocator.rs`);
4. **framebuffer** — ghi vào vùng nhớ firmware cấp (`src/splash.rs`).

Tất cả nằm trong 7 tệp được liệt kê ở `scripts/verify.py` (`FRAME_UNSAFE_FILES`),
mỗi tệp có lý do bằng tiếng Việt. Tổng: 29 khối, mỗi khối có chú thích an toàn.

## Nội dung

| Tệp | Việc |
|---|---|
| `src/main.rs` | `kmain`: serial → `YuiBootInfo` → profiler → bảng năng lực → màn hình chào |
| `src/boot.rs` | đọc multiboot 1 (x86_64) và device tree (ARM/RISC-V) → một `BootInfo` |
| `src/console.rs` | in ra cổng nối tiếp, có spinlock |
| `src/profiler.rs` | phân loại `may-yeu`/`can-bang`/`may-manh` + k/W, **cùng ngưỡng** với `kernel-services/profile.cpp` |
| `src/cap.rs` | khởi tạo `CapTable`, chứng minh dẫn xuất không mở rộng quyền + thu hồi bắc cầu |
| `src/splash.rs` | màn hình chào 5×7 trên framebuffer tuyến tính |
| `src/allocator.rs` | bump allocator 4 MiB |
| `src/arch/*` | x86_64 (multiboot + long mode), aarch64 (PL011), riscv64 (SBI) |

## Build & boot

```bash
rustup toolchain install nightly --component rust-src
./scripts/build-kernel.sh x86_64        # hoặc: make kernel ARCH=aarch64
./scripts/qemu-smoke.sh x86_64 --build  # hoặc: make qemu ARCH=aarch64
```

Ba dòng phải thấy trên serial: `yui: ready`, `Phan loai :`, `Tang 1    :`.

> **Trạng thái:** mã đã viết, **chưa từng được biên dịch** trong môi trường soạn
> thảo (không có `cargo`/`qemu`). `PLAN.md` đánh dấu P2.1–P2.3 là `[~]`, không
> phải `[x]`. CI job `kernel` + `qemu` là nơi xác nhận thật.
