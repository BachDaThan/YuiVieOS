# Điều khoản sử dụng YuiVieOS Beta (bản mẫu)

Cập nhật gần nhất: 2026-11-21 (đợt 9: thêm mục 12–19 — phạm vi miễn trừ cho tính năng AI/đa phương tiện, AI tự chủ, giọng nói, phát trực tiếp, lớp phủ game, gói người dùng, dữ liệu suy luận; trước đó đợt 6–8: ghi đĩa/cài song song, phần cứng, kết nối thiết bị, giới hạn trách nhiệm).

> Bản mẫu, **chưa được luật sư rà soát**. Xem thêm `LICENSE` (MIT) và `privacy_vi.md`. Đoạn giới hạn trách nhiệm dưới đây viết theo hướng "**tối đa trong phạm vi pháp luật cho phép**" — pháp luật có phần bắt buộc không thể loại trừ bằng điều khoản, nên không có tài liệu nào bảo đảm miễn trừ **mọi** trách nhiệm trong **mọi** trường hợp.

## 1. Bản chất dự án

**Dự án cá nhân, mã nguồn mở**, làm vì đam mê, phục vụ học tập và nghiên cứu kỹ thuật, chia sẻ miễn phí cho ai cần. Không phải sản phẩm thương mại, không phải dịch vụ: tác giả **không cam kết** duy trì, cập nhật, hỗ trợ, trả lời hay sửa lỗi, và có thể ngừng dự án bất cứ lúc nào.

## 2. Nguyên trạng, không bảo hành, tự chịu rủi ro

Phần mềm được cung cấp **nguyên trạng (AS-IS), "như hiện có"**, **không bảo đảm** dưới bất kỳ hình thức nào — về tính đúng đắn, an toàn, bảo mật, tính ổn định, tương thích với phần cứng/phần mềm của bạn, hay phù hợp cho mục đích nào. Đây là bản beta: **lỗi, mất dữ liệu, treo máy, không khởi động được là điều có thể xảy ra**. Việc cài, thử và dùng là **quyết định và rủi ro của riêng bạn**. **Hãy sao lưu mọi dữ liệu quan trọng trước khi thử trên máy thật.** Không dùng cho hệ thống an toàn tính mạng, thiết bị y tế, điều khiển giao thông/hàng không/công nghiệp, hay hạ tầng trọng yếu.

## 3. Ghi đĩa, cài song song, Live USB

- **Ghi vào đĩa là thao tác rủi ro nhất.** Cài đặt, tạo/sửa phân vùng, ghi bộ nạp có thể **làm mất dữ liệu, làm hệ điều hành khác (ví dụ Windows) không khởi động được, hoặc khoá ổ đã mã hoá (ví dụ BitLocker)** đòi mã khôi phục. Cập nhật của hệ điều hành khác hoặc thay đổi cấu hình khởi động/Secure Boot cũng có thể làm hệ thống không vào được — việc này đã từng xảy ra ở các hệ điều hành khác.
- **Bạn tự sao lưu** dữ liệu và **mã khôi phục** (BitLocker/Device Encryption) ra nơi ngoài máy **trước** khi cài. Tác giả không có bản sao nào của dữ liệu hay khoá của bạn.
- Chế độ **chạy thử từ USB** không ghi vào đĩa trong máy là lựa chọn ít rủi ro nhất; vẫn có thể có sự cố phần cứng/firmware.
- Trình cài đặt (khi có) hiện bản tóm tắt này và **yêu cầu bạn gõ câu xác nhận trước khi ghi byte đầu tiên**; việc bạn xác nhận nghĩa là bạn đã đọc và chấp nhận rủi ro. Bản điều khoản đã hiện được ghi (băm) vào nhật ký cục bộ trên máy bạn.
- Chia sẻ phân vùng với hệ điều hành khác: tệp của người dùng khác trên cùng máy có thể đọc được từ hệ này; bạn chịu trách nhiệm về dữ liệu của người khác lưu trên máy.

## 4. Phần cứng, driver, firmware

Hỗ trợ phần cứng **không được bảo đảm**; nhiều thiết bị sẽ chỉ chạy ở mức cơ bản hoặc không chạy. Việc đổi cài đặt BIOS/UEFI, đổi Secure Boot, đăng ký khoá ký vào firmware hay ghi firmware **có thể làm hỏng thiết bị hoặc làm mất bảo hành của nhà sản xuất**. Firmware, driver và khối nhị phân của nhà sản xuất (nếu có) theo giấy phép của họ, **do bạn tự tải** từ nguồn chính thức; tác giả không bảo đảm chúng.

## 5. Thành phần và gói bên thứ ba

Thành phần và gói bên thứ ba (ví dụ lớp tương thích ứng dụng, runtime container, gói do bạn tự thêm) thuộc nhà cung cấp của chúng, theo giấy phép của họ (xem `THIRD-PARTY.md`). Gói **không có chữ ký hợp lệ** chạy trong hộp cát và có cảnh báo; tác giả không chịu trách nhiệm về nguồn bạn tự tin cậy. Bạn tự tuân thủ giấy phép/điều khoản của phần mềm bạn chạy trên hệ điều hành này (ví dụ ứng dụng Windows/Android cần giấy phép của chính chúng).

## 6. Điều khoản của ứng dụng/game bạn chạy

Hệ điều hành này **không** cung cấp công cụ giả mạo thiết bị, giả vị trí/vùng, vượt kiểm tra tính toàn vẹn hay né chống gian lận. Nhiều ứng dụng/game cấm chạy trên máy ảo hoặc container, cấm nhiều tài khoản hay công cụ ngoài; vi phạm có thể bị khoá tài khoản và bạn tự chịu trách nhiệm. <!-- ranh-giới-ok: điều khoản: tuyên bố OS không cung cấp công cụ giả mạo/né kiểm tra -->

## 7. Nội dung do AI tạo và mô hình

Nội dung do AI tạo (khi tính năng AI được bật ở các phase sau) có thể sai, thiếu hoặc bịa. **Không dùng** cho chẩn đoán/điều trị y tế, tư vấn pháp lý, quyết định tài chính hay quyết định rủi ro cao khác. Mô hình và dữ liệu huấn luyện có nguồn và giấy phép riêng; bạn tự kiểm tra trước khi dùng cho mục đích của mình. Hành động nhạy cảm do AI đề xuất **cần bạn xác nhận**; việc bạn xác nhận là quyết định của bạn.

## 8. Mạng, Kho Ứng Dụng, kết nối thiết bị, lọc tên miền, mở cổng

- **Kho Ứng Dụng và gói tải thêm** (khi có): chỉ hiển thị thông tin và chỉ tới **nguồn chính thức** của nhà phát triển/kho mở; tác giả **không lưu bản sao** tệp cài đặt và không bảo đảm ứng dụng. Bộ dịch mã, dịch vụ Google và bản vá game đóng nguồn **do bạn tự tải** và theo giấy phép của chính chúng. Tệp ngoài chưa ký chạy trong hộp cát hạn chế; **bạn tự quyết định và tự chịu trách nhiệm khi cài**.
- **Kết nối thiết bị** (chép tệp, thông báo, chiếu màn hình, dùng chung chuột/phím, điều khiển từ xa — khi có): chỉ hoạt động khi bạn bật và ghép đôi; tính năng điều khiển từ xa **mặc định tắt**. Bạn chịu trách nhiệm về thiết bị bạn ghép và về việc xem/ghi hình/điều khiển máy của người khác (cần sự đồng ý của họ theo pháp luật nơi bạn sống).
- **Lọc tên miền quảng cáo/theo dõi** (khi có) mặc định tắt và có thể làm hỏng một số trang; bạn có thể tắt tổng, tắt theo ứng dụng, cho phép từng tên miền hoặc tạm tắt.
- **Mở dịch vụ ra Internet** (khi có): bạn tự chịu trách nhiệm về điều khoản của nhà mạng, của phần mềm máy chủ bạn chạy (ví dụ máy chủ trò chơi và thoả thuận người dùng cuối của trò chơi đó) và về dữ liệu của người khác lưu trên máy bạn.
- **Sao lưu/đồng bộ** (khi có): dữ liệu chỉ đi tới điểm đến **bạn tự cấu hình**; mất khoá khôi phục nghĩa là mất dữ liệu.

## 9. Giới hạn trách nhiệm

**Trong phạm vi tối đa mà pháp luật cho phép**, tác giả và những người đóng góp **không chịu trách nhiệm** về bất kỳ thiệt hại hay tổn thất nào — trực tiếp, gián tiếp, ngẫu nhiên, đặc biệt hay hệ quả; gồm (nhưng không giới hạn) mất dữ liệu, mất lợi nhuận, gián đoạn công việc, hỏng thiết bị/phần cứng, không khởi động được hệ điều hành khác, bị khoá ổ mã hoá, bị khoá tài khoản dịch vụ, lộ thông tin — phát sinh từ hoặc liên quan tới việc tải, cài, dùng hay không dùng được phần mềm và tài liệu này, **dù dựa trên hợp đồng, bồi thường ngoài hợp đồng hay căn cứ khác, và dù đã được báo trước khả năng xảy ra**. Điều khoản `LICENSE` (MIT) về từ chối bảo đảm và giới hạn trách nhiệm áp dụng song song.

## 10. Luật áp dụng, phần không loại trừ được, tách điều khoản

Pháp luật nơi bạn sống (ví dụ quy định về bảo vệ người tiêu dùng, bảo vệ dữ liệu cá nhân, hay trách nhiệm do lỗi cố ý/lỗi nặng) có thể có **quy định bắt buộc không thể loại trừ**. Với phần đó, điều khoản này chỉ áp dụng **trong mức pháp luật cho phép**, và **không** hạn chế các quyền mà pháp luật bắt buộc dành cho bạn. Nếu một phần của điều khoản này bị xem là không có hiệu lực, **phần còn lại vẫn giữ nguyên hiệu lực**. Nội dung tài liệu này **chưa được luật sư rà soát**.

## 11. Thay đổi điều khoản

Điều khoản có thể thay đổi; bản mới ghi ngày ở đầu tệp này. Tiếp tục dùng sau khi đổi nghĩa là bạn chấp nhận bản mới trong mức pháp luật cho phép; nếu không đồng ý, hãy ngừng dùng.

## 12. Phạm vi miễn trừ — áp dụng cho mọi tính năng (tóm tắt)

Triết lý của dự án: làm vì đam mê, chia sẻ miễn phí, **bạn là người quyết định và chịu trách nhiệm cuối cùng** cho việc bạn bật, nạp, chạy và công khai. **Trong phạm vi tối đa mà pháp luật cho phép**, tác giả và những người đóng góp **không chịu trách nhiệm** về:

- (a) hành động hay nội dung do AI tạo ra hoặc đề xuất — **kể cả khi bạn đã bấm xác nhận**;
- (b) nội dung, dữ liệu, bình luận, âm thanh, hình ảnh, tài liệu của **bên thứ ba** mà bạn đưa vào hệ thống;
- (c) việc bạn vi phạm điều khoản, bản quyền, quyền riêng tư hay luật của bên thứ ba (nền tảng, trò chơi, chủ giọng nói, chủ tài liệu…);
- (d) mô hình, gói, kỹ năng, thư viện, firmware mà **bạn tự tải hoặc tự nạp**;
- (e) những gì bạn **công khai** bằng hệ thống này (phát trực tiếp, đăng, gửi, phát giọng);
- (f) mất dữ liệu, hỏng thiết bị, khoá tài khoản, gián đoạn, thiệt hại tài chính hoặc uy tín phát sinh từ các điều trên.

Phần này **không** có nghĩa tác giả cố ý gây hại, và **không** hạn chế các quyền mà pháp luật bắt buộc dành cho bạn hay cho người thứ ba (xem mục 10). Các mục 13–19 mô tả cụ thể từng nhóm tính năng; tính năng ghi "khi có" nghĩa là **hiện chỉ là kế hoạch**, mô tả trong tài liệu không phải cam kết.

## 13. Trợ lý AI tự chủ, "phòng riêng" và hành động hệ thống (khi có)

- Tính năng AI chạy khi máy rảnh (phòng riêng) là **tuỳ chọn, mặc định TẮT**; bạn bật, bạn tắt hẳn được bất cứ lúc nào, và có nút dừng khẩn cấp.
- AI có thể **sai, thiếu, bịa, hoặc đổi hành vi** (kể cả sau khi tự vi chỉnh). Nội dung AI tạo lúc bạn vắng mặt **không phải ý kiến hay cam kết của tác giả**. Bạn có thể khôi phục về bản trước khi vi chỉnh.
- **Hành động nào tác động ra ngoài hộp cát** (ghi tệp thật, gửi, cài, xoá, đổi cấu hình) **cần bạn xác nhận trực tiếp trên thiết bị**; hộp thoại ghi rõ "AI đề xuất". Việc xác nhận là **quyết định của bạn**.
- Bạn có thể xem và xoá nhật ký "AI đã làm gì" trên máy; tác giả không có bản sao.
- Không dùng AI tự chủ cho hệ thống an toàn tính mạng, y tế, tài chính, pháp lý hay hạ tầng trọng yếu (như mục 2).

## 14. Giọng nói, nhân bản giọng, âm thanh và hình ảnh (khi có)

- Mô hình nghe/nói tải theo nhu cầu và có **giấy phép riêng** (có bản chỉ cho phép dùng phi thương mại); bạn tự đọc và tuân thủ. Tác giả không bảo đảm chất lượng, độ chính xác hay độ trễ.
- **Chỉ dùng giọng của chính bạn hoặc giọng có sự đồng ý của chủ giọng** (nên có bằng chứng đồng ý). **Cấm** dùng để mạo danh, lừa đảo, gây hiểu lầm về danh tính hay xâm phạm quyền của người khác. <!-- ranh-giới-ok: điều khoản cấm hành vi mạo danh, không cung cấp công cụ -->
- **Không gỡ** dấu nhận biết (watermark) mà mô hình tạo ra nếu có; nên ghi rõ nội dung giọng nói **do AI tạo** khi phát công khai.
- Ghi âm, ghi hình hay phân tích giọng/khuôn mặt/ảnh màn hình của **người khác** cần sự đồng ý của họ theo luật nơi bạn sống; bạn chịu trách nhiệm.

## 15. Phát trực tiếp, bình luận khán giả, video và nội dung của bên thứ ba (khi có)

- Bạn tự tuân thủ **điều khoản của từng nền tảng** (về bot, giọng tổng hợp, nội dung do AI tạo, bản quyền) và luật nơi bạn sống; tác giả **chưa đọc/không bảo đảm** điều khoản của nền tảng nào.
- Bình luận và thông tin của khán giả là **dữ liệu của họ**. Hệ thống thiết kế để **không lưu danh tính, không đưa vào ký ức dài hạn, không đọc to thông tin cá nhân**; bộ lọc có thể **sót**. Bạn chịu trách nhiệm về những gì phát ra.
- Hệ thống **không tải về hay lưu** video/phụ đề của nền tảng; chỉ xử lý âm thanh đang phát trên máy bạn khi bạn bật. Việc xem/nghe nội dung có bản quyền vẫn theo điều khoản của nội dung đó.

## 16. Lớp phủ, trợ giúp khi chơi game và dùng ứng dụng khác (khi có)

- Lớp phủ đọc **cây trợ năng và ảnh màn hình**; không đọc bộ nhớ của tiến trình khác. Mặc định là chế độ **"Chơi sạch"** (chỉ trợ giúp học tập, giải thích, công cụ cá nhân).
- Nhiều trò chơi/máy chủ **cấm** công cụ cho lợi thế (hiện vị trí khối, bản đồ ẩn…) và một số cơ chế chống gian lận coi **lớp phủ** là dấu hiệu vi phạm — có thể **khoá tài khoản**. Bật tính năng trợ giúp có lợi thế là **quyết định và rủi ro của riêng bạn**; hệ thống không có chế độ né kiểm tra và tác giả không chịu trách nhiệm về việc khoá tài khoản. <!-- ranh-giới-ok: điều khoản cảnh báo và từ chối trách nhiệm, không cung cấp chế độ né chống gian lận -->
- Công cụ tính hạt giống/bản đồ chỉ hỗ trợ phiên bản trò chơi ghi rõ trong tài liệu và chỉ nên dùng trên **thế giới của chính bạn**.

## 17. Gói nhân vật, gói tri thức, kỹ năng và nội dung bạn tự nạp (khi có)

- Gói nhân vật (`.yuic`), gói tri thức (`.yuik`), kỹ năng, mô hình giọng, ảnh/hoạt hoạ do **bạn tự nạp hoặc tự tạo**: bạn chịu trách nhiệm về **bản quyền, giấy phép, sự đồng ý** của người có liên quan (hoạ sĩ, chủ giọng, chủ tài liệu). Hệ thống chạy gói không có chữ ký trong hộp cát **chỉ-dữ-liệu** và hiện cảnh báo nguồn gốc; việc bạn bấm tiếp tục nghĩa là bạn chấp nhận rủi ro.
- Gói tri thức là **bản chắt lọc từ tài liệu nguồn**: dùng **trên máy bạn**; **không chia sẻ lại** nếu nội dung nguồn có bản quyền hoặc điều khoản không cho phép.
- Tác giả **không lưu bản sao** gói hay nội dung của bạn và không có cách khôi phục hộ.

## 18. Dữ liệu suy luận về trạng thái của bạn (khi có)

Các tính năng suy luận cảm xúc, mức tải nhận thức, nhịp gõ hay thói quen dùng máy là **tuỳ chọn, mặc định TẮT, xử lý trên máy, không lưu dữ liệu thô, không gửi đi**. Kết quả có thể **sai**; **không** dùng cho chẩn đoán/điều trị y tế hay tâm lý, và **không** dùng để đánh giá, theo dõi hay giám sát người khác.

## 19. Tính năng "khi có", bản beta và thay đổi

Mô tả tính năng trong kế hoạch, README hay tài liệu thiết kế là **định hướng**, không phải cam kết rằng tính năng tồn tại hay chạy đúng. Con số hiệu năng chỉ có giá trị khi có dòng đo thật tương ứng. Việc dùng tính năng mới sau khi điều khoản đổi theo mục 11.
