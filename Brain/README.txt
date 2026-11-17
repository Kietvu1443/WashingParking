LAB 04 DEEP LEARNING SMART PARKING - CHƯƠNG TRÌNH TRÊN MÁY TÍNH
================================================================

LÀM THEO THỨ TỰ SỐ Ở ĐẦU TÊN TỆP (chạy bằng python, mọi hệ điều hành)

  Bước 0  Nạp firmware ESP32       Xem ESP32\README_NAP_FIRMWARE.txt (một firmware dùng cho cả 2 mô hình)
  Bước 1  python run.py --setup            Cài môi trường .venv và thư viện để chạy (1 lần trên mỗi máy, cần Internet)
  Bước 2  python run.py            Chạy chương trình AI, mở http://localhost:8000 (vẽ 4 ô ở trang /ai)
  Bước 3  3_cai_dat_huan_luyen.bat Chỉ máy dùng để huấn luyện: cài PyTorch (GPU hoặc CPU) và Ultralytics
  Bước 4  4_huan_luyen_xe.bat      Huấn luyện lại mô hình xe từ thư mục dataset (tắt python run.py trước)
  Bước 5  5_huan_luyen_bien_so.bat Huấn luyện lại mô hình biển số từ thư mục dataset_bien_so
  Sau bước 4, 5: chạy lại python run.py là dùng mô hình mới.

YÊU CẦU
  Python 3.9, 3.10, 3.11 hoặc 3.12 (https://www.python.org/downloads/, khi cài đánh dấu
  "Add python.exe to PATH"). Python 3.13 trở lên chưa hỗ trợ.
  Windows: đặt thư mục ở đường dẫn ngắn (tối đa 100 ký tự), ví dụ C:\SmartParking\SmartPK_PC.
  Chạy nhận diện chỉ cần CPU. Huấn luyện nên có card NVIDIA (không có vẫn chạy được, chậm hơn khoảng 10 lần).

THÊM THAM SỐ KHI CẦN
  python run.py --cam 1 --port COM5          chọn camera số 1 và cổng ESP32 COM5
  python run.py --web-pass MAT_KHAU          đặt mật khẩu cho trang web điều khiển
  4_huan_luyen_xe.bat --epochs 150           huấn luyện lâu hơn
  python run.py <tệp.py> [tham số]                chạy tệp Python bất kỳ bằng môi trường .venv,
                                             ví dụ: python run.py tao_anh_bien_gia.py --so-anh 1500
  Camera và cổng ESP32 được tự tìm; sai thì dùng --cam và --port.

CÁC TỆP CÒN LẠI (không cần mở khi chạy)
  pc_ai_camera.py                  Chương trình AI: camera, YOLO tìm xe và biển số, đọc biển, web, UART
  train_xe.py, train_bien_so.py    Mã huấn luyện (bước 4, 5 gọi tới)
  tao_anh_bien_gia.py              Tạo ảnh biển số giả để bổ sung dữ liệu
  python run.py, python run.py                Chạy tệp Python bằng .venv; tự cài lại khi cần
  requirements.txt                 Phiên bản thư viện để chạy; requirements-train.txt để huấn luyện
  xe_best.onnx, bien_so.onnx       Mô hình đang dùng; xe_best.pt, bien_so.pt để huấn luyện tiếp
  rec.onnx, dec.onnx               Mô hình đọc ký tự biển số
  slots.json, background.png       Vị trí 4 ô và ảnh nền bãi trống (vẽ lại khi dời camera)
  parking.db, plates               Danh sách chủ xe, nhật ký và ảnh biển (tự tạo khi chạy)
  dataset, dataset_bien_so         Ảnh và nhãn để huấn luyện
  HUONG_DAN_PC.txt                 Hướng dẫn chi tiết các chức năng trên trang web

KHI ĐỌC ĐƯỢC BIỂN SỐ
  Chương trình tra parking.db rồi gửi lệnh PLATE xuống ESP32; LCD hiện 5 giây:
    xe đã đăng ký : dòng 1 "BIEN 59A1-815.91", dòng 2 tên chủ xe không dấu
    xe lạ         : dòng 1 "BIEN 51F-999.99",  dòng 2 "XE LA CHUA DK"
  Nhiều biển đọc liền nhau thì xếp hàng đợi, hiện lần lượt mỗi biển đủ 5 giây (không đè nhau).

CHÉP SANG MÁY KHÁC
  Chép cả thư mục, có thể bỏ thư mục .venv (máy mới tự tạo lại). Mô hình .onnx chạy được trên mọi máy.
  Sau khi chép nên vẽ lại 4 ô và chụp lại ảnh nền vì vị trí camera mỗi bãi mỗi khác.

LỖI THƯỜNG GẶP
  Không tìm thấy Python 3.9 đến 3.12   cài Python 3.12, đánh dấu Add python.exe to PATH
  Đường dẫn thư mục quá dài            chép thư mục ra chỗ ngắn hơn, ví dụ C:\SmartParking\
  Không mở được camera                 tắt Zoom, ứng dụng Camera; thử --cam 0 hoặc --cam 1
  Không kết nối được ESP32             đóng Arduino IDE, Serial Monitor; thử --port COMx
  Linux không mở được cổng USB         sudo usermod -aG dialout $USER rồi đăng xuất, đăng nhập lại
  Huấn luyện báo dùng GPU: False       máy không có card NVIDIA hoặc driver cũ; vẫn huấn luyện bằng CPU được
