# EtherCat_Rasp4

Raspberry Pi 4 làm EtherCAT master (SOEM) điều khiển driver bước Leadshine
**DM3E-556** (CiA 402) với giao diện web trên cổng 8080.

> ## ⚠️ AN TOÀN
> **Giao diện web KHÔNG thay thế nút dừng khẩn cấp phần cứng.**
> Máy phải có E-stop phần cứng ngắt nguồn công suất 24 V của driver/motor,
> độc lập với Pi, mạng và phần mềm này. Nút STOP trên web chỉ là dừng
> bằng phần mềm và có thể không hoạt động khi Pi, WiFi hoặc trình duyệt gặp sự cố.

## Trạng thái

| Bước | Nội dung | Trạng thái |
|---|---|---|
| 1 | Khung repo, SOEM, `ecat-scan` (quét bus, đọc SDO) | ✅ |
| 2 | Vòng lặp 1 ms (DC Sync0), đo chu kỳ/jitter, đưa slave lên OP | ✅ |
| 3 | Máy trạng thái CiA 402, profile CSP, heartbeat | ⏳ |
| 4 | IPC + web server + giao diện | ⏳ |

## Phần cứng

- `eth0` (onboard) dành riêng cho bus EtherCAT, cắm thẳng vào cổng **IN** của driver.
- SSH/web đi qua `wlan0` hoặc Tailscale.
- Driver/motor dùng nguồn 24 V riêng.

## Chuẩn bị hệ thống (một lần)

```sh
sudo apt install cmake
# Không cho NetworkManager đụng vào eth0 (DHCP/IPv6 lên bus EtherCAT)
printf '[keyfile]\nunmanaged-devices=interface-name:eth0\n' | \
  sudo tee /etc/NetworkManager/conf.d/99-ethercat.conf
sudo nmcli general reload conf
```

Mỗi lần khởi động (chưa có dịch vụ systemd):

```sh
sudo scripts/setup-net.sh eth0
```

## Build

```sh
git submodule update --init
cmake -S rt -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

## Chạy

Quét bus và đọc thông tin slave (chỉ lên PRE-OP, không enable trục):

```sh
sudo ./build/ecat-scan eth0
```

Chạy vòng lặp RT (bước 2: lên OP, đo jitter, trục luôn disable):

```sh
sudo ./build/ecat-rt -c config/axis.ini -t 60   # -t 0 = chạy đến Ctrl+C
#   -C 3   CPU cho thread RT (mặc định 3, -1 = không ghim)
#   -p 80  ưu tiên SCHED_FIFO
```

Mỗi giây in: trạng thái EtherCAT, chu kỳ trung bình/min/max, latency đánh thức
xấu nhất, sai lệch DC, WKC, statusword, 603F, chế độ, vị trí. Khi thoát in
tổng kết latency.

## Cấu hình

`config/axis.ini`: giao diện bus, chu kỳ, giới hạn tốc độ/gia tốc và timeout
heartbeat. Các giới hạn này được tiến trình RT áp đặt; web không vượt qua được.

## Cấu trúc

```
config/        cấu hình trục và giới hạn
DM3E/          manual driver (+ esi/ cho file ESI)
rt/            tiến trình thời gian thực (C, SOEM) và tool
scripts/       script chuẩn bị mạng, ghi chú tinh chỉnh RT
third_party/   SOEM (submodule, ghim v2.0.1)
```

## Giấy phép

SOEM v2 dùng GPLv3 (hoặc giấy phép thương mại), nên phần mềm build ra
cũng phải tuân theo GPLv3 nếu phân phối.
