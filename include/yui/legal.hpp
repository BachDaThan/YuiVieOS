// include/yui/legal.hpp — nhãn "AS-IS" chuẩn, một nguồn duy nhất (P7.9).
//
// Mọi nơi người dùng nhìn thấy (banner shell, lệnh `phiênbản`, …) lấy câu từ
// đây; `scripts/check_legal.py` (chạy trong verify.sh) kiểm câu này còn đủ ý
// với khối AS-IS ở đầu README.md, và kiểm các nơi hiển thị thật sự dùng nó.
// Đổi câu ⇒ đổi cùng lúc README + nhân (kernel-frame/src/main.rs, bản ASCII).
#pragma once

namespace yui::legal {

// Tiếng Việt: bản beta, miễn phí, nguyên trạng, không bảo hành, tự chịu trách
// nhiệm, trỏ tới điều khoản và chính sách riêng tư.
inline constexpr const char* kAsIsVi =
    "Bản beta miễn phí, cung cấp nguyên trạng (AS-IS), không bảo hành — "
    "bạn tự chịu trách nhiệm khi dùng. Điều khoản: docs/terms_vi.md · "
    "Riêng tư: docs/privacy_vi.md";

inline constexpr const char* kAsIsEn =
    "Free beta, provided AS-IS without warranty — use at your own risk.";

}  // namespace yui::legal
