# reference/ — cài đặt tham chiếu (sẽ bị thay ở Phase P5)

Ba tệp ở đây **không phải** lõi AI. Chúng tồn tại để hợp đồng của cầu nối có
thể kiểm thử trước khi có `libyui.so` thật:

| Tệp | Vai trò trong P5 |
|---|---|
| `graph.cpp` | thay bằng `yui_graph` (Adaptive-MoE) — cùng thao tác `upsert/link/query/path/stats/export/import` |
| `vault.cpp` | thay bằng `yui_vault` — **khác định dạng blob**: giữ `plain` để migrate, không giả định tương thích |
| `engine.cpp` | thay bằng `yui.cpp` (q4/t2/BITCOS, `.yui v3`) — hiện chỉ là số học q4 + SIMD để đo hiệu năng khung |

Quy tắc khi thay: **không** sửa `daemon/`, **không** đổi tên phương thức JSON-RPC,
**không** đổi mã lỗi C-ABI. Chỉ cắm thư viện mới vào sau `Service` (xem
`docs/PLAN.md` P5).
