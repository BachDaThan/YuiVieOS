# ai-bridge-yuid — Tầng 4: Cổng Chờ Dịch Vụ Trí Tuệ Lõi

Đây **không phải** lõi mô hình AI. Đây là *cầu nối*: mọi giao diện mà `yuid`
(phát triển độc lập ở `Adaptive-MoE` và `Y-App`) cần để nói chuyện với
YuiVieOS Beta — và ngược lại. Lõi AI thật được cắm vào ở **Phase P5**.

## Ba mặt tiền đã dựng sẵn

| Mặt tiền | Đường dẫn | Trạng thái |
|---|---|---|
| IPC JSON-RPC | `/run/yuid.sock` (SOCK_STREAM, 0600, một JSON/dòng) | ✅ chạy |
| C-ABI FFI | `libyui.so` / `libyui.a`, header `include/yui/yui.h` | ✅ chạy, kiểm từ C11 |
| MCP (stdio) | `yuid --mcp`, giao thức `2024-11-05`, 8 công cụ | ✅ chạy |

## Ba hợp đồng mà lõi AI phải tuân

1. **Telemetry Hook** — `telemetry.read` (JSON-RPC) và `yui_telemetry()` (C-ABI)
   trả pin %, nhiệt độ CPU/GPU, RAM trống, chế độ máy; kèm khuyến nghị `k/W`
   (`k=1, W=16` khi máy yếu/pin thấp; `k=2..4` khi cắm sạc/máy mạnh).
2. **OS Control MCP** — 5 công cụ có phân quyền và **có xác nhận của người dùng**:
   `sys_power`, `sys_scheduler`, `sys_app_stripper`, `sys_proc`, `sys_search_vault`.
3. **Nạp mô hình** — `libyui` `mmap` trực tiếp tệp `.yui v3`; bridge chỉ chuyển
   handle + đường dẫn, không đọc/giải mã trọng số.

Đặc tả đầy đủ: [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) §4 và
[`docs/PROTOCOL.md`](../docs/PROTOCOL.md).

## Nội dung

| Thư mục | Việc |
|---|---|
| `daemon/` | `service.cpp` (dispatch JSON-RPC), `server.cpp` (socket + MCP), `main.cpp` (`yuid`) |
| `capi.cpp` | Toàn bộ bề mặt C-ABI |
| `reference/` | Cài đặt tham chiếu đồ thị/engine/vault — **sẽ được thay bằng `libyui` ở P5** |

Cài đặt tham chiếu tồn tại để: (a) hợp đồng có thể kiểm thử ngay hôm nay,
(b) hệ thống vẫn dùng được khi chưa cài AI, (c) là đặc tả thực thi cho lõi thật.
