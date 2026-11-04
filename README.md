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
| 3 | Máy trạng thái CiA 402, profile CSP, heartbeat, IPC, `ecat-ctl` | ✅ |
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

Chạy tiến trình RT (trục luôn khởi động ở trạng thái disable):

```sh
sudo ./build/ecat-rt -c config/axis.ini      # Ctrl+C để dừng (motor bị cắt điện)
#   -t N   chạy N giây rồi thoát (0 = mãi)
#   -C 3   CPU cho thread RT (mặc định 3, -1 = không ghim)
#   -p 80  ưu tiên SCHED_FIFO
#   -q     không in log mỗi giây
```

Điều khiển từ dòng lệnh khi chưa có web (user phải thuộc nhóm `[ipc] group`):

```sh
./build/ecat-ctl status            # hoặc: watch
./build/ecat-ctl enable            # disable | reset (fault reset) | stop
./build/ecat-ctl jog 30 2          # 30 rpm trong 2 s rồi STOP
./build/ecat-ctl jog -60 1 nohb    # ngừng gửi mà không STOP -> test heartbeat
```

`ecat-scan` từ chối chạy khi `ecat-rt` đang chạy (quét bus sẽ đưa slave về INIT).

## Hành vi an toàn (thực thi trong `ecat-rt`, không phụ thuộc web)

| Tình huống | Phản ứng |
|---|---|
| Khởi động `ecat-rt` | Trục disable, không tự enable |
| Jog không được gửi lại trong `heartbeat_timeout_ms` | Giảm tốc với `stop_decel_rpm_s` rồi dừng (vẫn enable) |
| STOP | Giảm tốc dừng; **chốt**: bỏ qua JOG đến khi client ngừng gửi jog (nhả nút) |
| Disable khi đang chạy | Giảm tốc dừng rồi mới cắt điện motor |
| Mất bus (WKC sai 3 chu kỳ liên tiếp hoặc slave rời OP) | Bỏ enable; khi bus khôi phục trục vẫn disable, phải bấm Enable lại |
| Driver báo Fault | Bỏ enable; cần Fault Reset rồi Enable |
| Enable không thành công trong 1 s | Hủy yêu cầu enable |
| Tốc độ jog vượt `max_jog_rpm` | Bị kẹp |
| `ecat-rt` thoát (Ctrl+C/SIGTERM) | Gửi controlword 0, đưa slave về INIT |
| Driver mất nguồn rồi có lại | Cấu hình lại (dòng đỉnh, CSP, Sync0), trục vẫn disable |

## Cấu hình

`config/axis.ini`: giao diện bus, chu kỳ, xung/vòng (phải khớp 0x2001 của driver),
dòng đỉnh (ghi vào 0x2000 mỗi lần cấu hình, chỉ RAM), giới hạn tốc độ/gia tốc,
timeout heartbeat và nhóm được phép gửi lệnh. Các giới hạn được tiến trình RT
áp đặt; web không vượt qua được. Đổi cấu hình cần khởi động lại `ecat-rt`.

## IPC

Xem `shared/ipc_layout.h`.

- Trạng thái: POSIX shm `/dev/shm/ecat-rt-status` (seqlock, chỉ `ecat-rt` ghi, mọi user đọc được).
- Lệnh: Unix datagram socket `/run/ecat-rt/cmd.sock` (root:`[ipc] group`, 0660),
  `ecat-rt` đọc non-blocking mỗi chu kỳ nên web chậm/treo không làm trễ vòng lặp.

## Cấu trúc

```
config/        cấu hình trục và giới hạn
DM3E/          manual driver
rt/src/        ecat-rt: bus, cia402, profile, ipc, config
rt/tools/      ecat-scan, ecat-ctl
shared/        ipc_layout.h (dùng chung C và Python)
scripts/       script chuẩn bị mạng, ghi chú tinh chỉnh RT
third_party/   SOEM (submodule, ghim v2.0.1)
```

## Giấy phép

SOEM v2 dùng GPLv3 (hoặc giấy phép thương mại), nên phần mềm build ra
cũng phải tuân theo GPLv3 nếu phân phối.
