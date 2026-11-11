# WashingParking – Trạm rửa xe & gửi xe thông minh

Dự án mô phỏng một trạm **rửa xe + gửi xe** có AI: camera đọc biển số, nhận diện khoang trống, đo độ bẩn, tự trừ tiền ví khi xe ra cổng.

Hiện tại repo có **bản demo 3D chạy trên trình duyệt** (thư mục `demo/`). Backend (FastAPI), giao diện Next.js và AI thật **chưa được xây**; toàn bộ "AI" trong demo là **giả lập** (hiển thị khung quét và phần trăm tin cậy cho giống thật).

## 1. Chạy demo

Demo là trang web tĩnh, không cần build hay cài thư viện. Chỉ cần mở bằng một web server tĩnh (không mở trực tiếp file `index.html` bằng đường dẫn `file://` vì trình duyệt sẽ chặn module JavaScript).

```bash
# Cách 1: Python
python -m http.server 8080 -d demo

# Cách 2: Node
npx serve demo
```

Sau đó mở http://localhost:8080 (nếu dùng `npx serve` thì mở địa chỉ nó in ra).

Chạy kiểm thử phần logic (không cần trình duyệt, cần Node.js):

```bash
node demo/logic.test.mjs
```

Kết quả đúng: dòng cuối là `20 nhóm kiểm thử đạt`.

## 2. Cách dùng demo

Thanh dưới cùng:

| Nút | Tác dụng |
|---|---|
| ▶ Chạy minh họa | Chạy kịch bản có sẵn (8 xe lần lượt vào, đủ các tình huống). Bấm lại để dừng |
| ＋ Thêm xe | Tự thêm một xe: chọn loại xe, biển số, khách đăng ký hay vãng lai, dịch vụ, số dư ví |
| Ⅱ / ▶ | Tạm dừng / tiếp tục |
| ↺ | Đặt lại (có sẵn vài xe mẫu) |
| Tốc độ | 0,5× đến 4× |
| Aa | Ẩn / hiện nhãn biển số trong cảnh 3D |

Thanh trên cùng có các **góc nhìn**: Tổng quan, Cổng vào, Khu rửa, Bãi chờ, **Bãi đỗ**, Cổng ra. Có thể xoay, kéo, zoom bằng chuột.

**Bấm vào xe, khoang rửa, bãi đỗ hoặc cổng** trong cảnh 3D để mở bảng bên phải. Với xe, bảng hiển thị như màn hình app của khách (tiến trình, hóa đơn, số dư ví) và có nút điều khiển thủ công (Bắt đầu rửa, Đưa ra bãi chờ, Lấy xe, ...). Ở cổng ra, khi barie hạ, thu ngân bấm **"Đã thu tiền mặt"**.

Cửa sổ **Camera AI** (góc trái) tự hiện khi có sự kiện: xe qua cổng vào, xe được gán khoang, camera trong khoang đo độ bẩn, xe được gán chỗ đỗ, xe qua cổng ra.

Nút **Chế độ: Mô phỏng / Trực tiếp**: chế độ "Trực tiếp" chỉ đọc `/api/bays` từ backend. Backend chưa có nên chế độ này sẽ báo chưa kết nối; dùng chế độ **Mô phỏng**.

## 3. Quy trình nghiệp vụ trong demo

Xe vào cổng → camera đọc biển số → khớp tài khoản (khách đăng ký) hoặc cấp vé vãng lai.

Khách đăng ký chọn 1 trong 3 dịch vụ. Khách vãng lai chỉ được **Rửa xe** (gói cơ bản, không có app/ví).

1. **Rửa xe**: vào khoang (hết khoang thì chờ ở hàng chờ, tối đa 4 xe) → camera quét độ bẩn → rửa → nhân viên đưa ra **bãi chờ** (tối đa 6) → ra cổng.
2. **Gửi xe**: vào thẳng **bãi đỗ** (10 chỗ ô tô, 2 chỗ xe tải) → hết thời gian gửi (ngẫu nhiên 20–40 giây mô phỏng) hoặc bấm **Lấy xe** → ra cổng. Bãi đỗ đầy thì từ chối xe.
3. **Rửa + gửi**: rửa xong chuyển sang bãi đỗ; nếu bãi đỗ đầy thì rơi về bãi chờ (không tính phí gửi).

Trạng thái đơn: `WAITING_BAY → IN_BAY → WASHING → WASHED → IN_WAITING_LOT` hoặc `PARKED → AT_EXIT → COMPLETED`.

### Độ bẩn (AI) quyết định thông số rửa

Camera trong khoang quét độ bẩn (0–100%) trong lúc chờ bắt đầu rửa. **Giá không đổi**, chỉ đổi thông số và thời gian rửa:

Trên mô hình 3D, xe **bẩn thật sự**: sơn và kính ngả nâu, có các mảng bùn bám thân xe (độ bẩn càng cao càng nhiều). Xe sạch dần theo tiến độ rửa và hết hẳn khi rửa xong. Xe chỉ gửi (không rửa) giữ nguyên độ bẩn.


| Mức | Độ bẩn | Áp suất | Nhiệt độ | Nước | Thời gian rửa |
|---|---|---|---|---|---|
| Sạch | 0–25 | 80 bar | 25°C | 60 L | ×0,8 |
| Vừa | 26–50 | 100 bar | 35°C | 100 L | ×1,0 |
| Bẩn | 51–75 | 120 bar | 45°C | 140 L | ×1,3 |
| Rất bẩn | 76–100 | 140 bar | 55°C | 180 L | ×1,6 |

Xe tải dùng nước ×1,5.

### Giá

| Xe | Gói | Giá |
|---|---|---|
| Ô tô | Rửa cơ bản | 80.000đ |
| Ô tô | Bọt tuyết + hút bụi nội thất | 150.000đ |
| Ô tô | Gói toàn diện | 250.000đ |
| Xe tải | Rửa ngoài | 150.000đ |
| Xe tải | Rửa ngoài + xịt gầm | 250.000đ |

Phí gửi xe: **20.000đ/giờ (ô tô), 40.000đ/giờ (xe tải)**, làm tròn lên theo giờ, tối thiểu 1 giờ. Trong demo **1 giây mô phỏng = 10 phút thực**.

### Thanh toán tại cổng ra

Camera cổng ra đọc biển số rồi:

- Khách có tài khoản và **ví đủ tiền** (rửa + gửi) → ví app **tự trừ**, barie không hạ, xe đi thẳng.
- **Khách vãng lai** hoặc **ví không đủ** → **hạ barie**, thu ngân thu tiền mặt rồi mới mở.

Khi thêm xe, số dư ví chọn **Đủ (500.000đ)** hoặc **Thiếu (20.000đ)** để thử hai trường hợp. Cổng ra chỉ xử lý **một xe mỗi lần**.

## 4. Cấu trúc thư mục

```
WashingParking/
├── demo/                  Bản demo 3D (trang tĩnh, Three.js)
│   ├── index.html         Giao diện
│   ├── style.css          Kiểu dáng
│   ├── logic.js           Máy trạng thái đơn hàng (thuần logic, không dính DOM/3D)
│   ├── logic.test.mjs     Kiểm thử cho logic.js
│   ├── world.js           Dựng cảnh 3D tĩnh (đường, khoang, bãi chờ, bãi đỗ, cổng)
│   ├── models.js          Mô hình xe, nhãn chữ
│   ├── app.js             Vòng lặp, di chuyển xe, camera AI giả lập, bảng điều khiển
│   └── vendor/            Three.js + OrbitControls (giấy phép MIT)
├── docs/superpowers/
│   ├── specs/             Đặc tả thiết kế hệ thống (mục 4b là máy trạng thái chuẩn)
│   └── plans/             Kế hoạch triển khai backend/frontend (cần chỉnh lại)
└── idea/                  Kịch bản gốc của hệ thống
```

**Nguyên tắc:** mọi luật nghiệp vụ nằm ở `logic.js`. `app.js` chỉ vẽ lại và gọi hàm của `logic.js`. Khi viết backend, cần cho kết quả **tương đương** `logic.js` (xem `docs/.../2026-11-11-washing-parking-system-design.md`, mục 4b).

## 5. Muốn sửa thì sửa ở đâu?

| Muốn đổi | Sửa file | Chỗ cần tìm |
|---|---|---|
| Giá gói rửa, thời gian rửa | `demo/logic.js` | `PACKAGES` |
| Bảng độ bẩn → áp suất/nhiệt độ/nước | `demo/logic.js` | `DIRT_LEVELS` |
| Phí gửi xe, thời gian gửi, tốc độ quy đổi | `demo/logic.js` | `PARK_RATE`, `CFG` |
| Số chỗ đỗ / loại chỗ | `demo/logic.js` + `world.js` | `PARK_DEFS` |
| Vị trí khoang, bãi đỗ, cổng trong cảnh 3D | `demo/world.js` | các hằng số ở đầu file |
| Kịch bản "Chạy minh họa" | `demo/app.js` | `SCRIPT` |
| Chữ hiển thị / thông báo lỗi | `demo/app.js` | `REASONS`, các hàm `...Html` |

Sau khi sửa `logic.js`, luôn chạy lại `node demo/logic.test.mjs`.

Nếu sau khi cập nhật code mà trang báo lỗi lạ hoặc không đổi gì, trình duyệt đang dùng file cũ trong bộ nhớ đệm: bấm **Ctrl+F5** (tải lại cứng).

## 6. Hạn chế hiện tại

- AI chỉ là **giả lập** (không dùng mô hình thật); độ bẩn là số ngẫu nhiên.
- **Xe máy chưa nằm trong phạm vi** demo (chỉ ô tô và xe tải).
- Chưa có backend, cơ sở dữ liệu, đăng nhập, nạp tiền ví. Dữ liệu mất khi tải lại trang.
- Chưa có làn riêng/đặt chỗ gửi xe trước, cảnh báo gửi quá giờ.

## 7. Hướng phát triển

- Backend FastAPI + WebSocket, đồng bộ với `logic.js`.
- AI thật: nhận diện xe/biển số (YOLO + OCR), ước lượng độ bẩn từ ảnh camera.
- Nạp tiền ví, thông báo khi số dư thấp trước lúc ra cổng.
- Đặt chỗ gửi xe trước, hiển thị thời gian chờ ước tính trong app.
