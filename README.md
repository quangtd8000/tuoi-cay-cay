# Hệ thống tưới cây tự động 

Dự án Arduino tưới cây tự động dựa trên độ ẩm đất và thời gian trong ngày.

## Phần cứng sử dụng
- Arduino Uno
- Cảm biến độ ẩm đất FC-28
- DHT11
- RTC DS3231
- Bơm nước DC
- L298N 

## Chức năng chính
- Không tưới ban đêm
- Tưới theo mức độ ẩm đất
- Dừng tưới khi hết nước
- Có thể mở rộng nhiều bơm chạy song song

## Logic tưới (tóm tắt)
- Độ ẩm < 20% → tưới ngay
- 40% – 60% → tưới nhẹ
- > 60% → không tưới

## Trạng thái
- Code được giữ lại để tham khảo 
