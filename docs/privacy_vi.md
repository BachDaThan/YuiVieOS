# Chính sách quyền riêng tư YuiVieOS Beta (bản mẫu)

> Bản mẫu, **chưa được luật sư rà soát**; chưa đối chiếu từng điều của Luật Bảo vệ dữ liệu cá nhân 91/2025/QH15 và văn bản hướng dẫn.

1. **Dữ liệu nằm trên máy của bạn.** Theo thiết kế hiện tại, các thành phần trong repo này không gửi dữ liệu của bạn tới máy chủ nào của tác giả. Tác giả không nhận bản sao dữ liệu của bạn.
2. **Nhật ký kiểm toán (`audit.log`)** ghi hành động của tiến trình (ví dụ tên tiến trình, quyền được cấp/bị từ chối, tên miền bị chặn). Đây có thể là thông tin về hành vi của bạn. Nhật ký chỉ nằm trên máy; tự xoay vòng khi vượt kích thước đặt trước (`audit_max_bytes`), có lệnh xoá sạch (`nhậtký xoá` / `audit clear`) và tuỳ chọn băm tên miền (P7.5, đã có test). Bạn cũng có thể xoá tệp bất cứ lúc nào.
3. **Kết nối mạng ra ngoài** chỉ xảy ra khi bạn hoặc phần mềm bạn chạy yêu cầu và được bộ lọc cho phép. Khi bạn tự cài thành phần cần mạng, thành phần đó có chính sách riêng; danh sách sẽ được bổ sung tại đây khi có.
4. **Kho Ứng Dụng, proxy và lọc tên miền** (khi có): tải từ nguồn chính thức làm lộ địa chỉ IP của bạn cho nguồn đó. Nếu bạn cấu hình proxy cho một phiên Android, **bên vận hành proxy thấy được lưu lượng không mã hoá**; thông tin đăng nhập proxy không ghi vào nhật ký. Danh sách chặn tên miền bạn bật được tải từ nguồn của danh sách đó. Dịch vụ bạn mở ra Internet (nếu có) có thể bị quét từ bên ngoài; dữ liệu của người khác lưu trên máy bạn là trách nhiệm của bạn.
5. **Quyền của bạn:** xem, sao chép, xoá dữ liệu trên máy, hoặc ngừng dùng bất cứ lúc nào.
6. **Trẻ em:** ngưỡng tuổi và đồng ý của người giám hộ cần xác nhận theo luật hiện hành (chưa xác nhận).
7. **Cài đặt và quét phần cứng** (khi có): trình cài đặt đọc bảng phân vùng, kích thước đĩa và dấu hiệu ổ mã hoá để cảnh báo trước khi ghi; công cụ quét phần cứng (`yui-hw`) đọc danh sách thiết bị. **Kết quả chỉ nằm trên máy**; báo cáo xuất ra tệp do bạn tự chọn gửi (hoặc không gửi). Không tự gửi đi đâu.
8. **Kết nối thiết bị** (khi có): nội dung thông báo/tin nhắn từ điện thoại chỉ giữ trong bộ nhớ, **không ghi vào nhật ký kiểm toán**; bảng tạm chỉ đồng bộ khi bạn bật. Tệp gửi qua mạng nội bộ được mã hoá đầu cuối giữa các thiết bị bạn ghép. Mật khẩu Wi-Fi, khoá SSH và khoá khác nằm trong kho bí mật của máy, **không** vào nhật ký và không đưa cho mô hình AI.
9. **Điều khiển từ xa, micro, camera** (khi có): mặc định tắt; mở bằng xác nhận của bạn, có thời hạn, và **luôn có đèn/viền báo** do hệ điều hành vẽ trong lúc đang mở. Nhật ký chỉ ghi *thời điểm bắt đầu/kết thúc phiên*, không ghi nội dung. Nhập giọng nói (nếu có) là bấm-để-nói, xử lý trên máy.
10. **Sao lưu và đồng bộ** (khi có): dữ liệu được mã hoá trước khi rời máy và chỉ gửi tới điểm đến **bạn tự khai**; dự án không có điểm đến mặc định. Mất khoá khôi phục nghĩa là không đọc lại được bản sao lưu — tác giả không giữ khoá.
11. **Dữ liệu của người khác trên máy dùng chung** (ví dụ phân vùng chung với hệ điều hành khác hoặc người dùng khác): bạn chịu trách nhiệm về dữ liệu của họ; hệ này đọc NTFS ở chế độ chỉ-đọc theo thiết kế.
12. **Huấn luyện/dữ liệu AI** (ở repo `Adaptive-MoE`, khi bạn tự chạy): dữ liệu web do bạn tự thu thập bằng khoá của bạn; nguồn và ngày thu thập được ghi cục bộ; bạn tự xem xét giấy phép/điều khoản của nguồn trước khi công bố.
13. Liên hệ: mở Issue trên repo GitHub của dự án.
