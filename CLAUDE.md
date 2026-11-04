# CLAUDE.md

Pi 4 (hostname musicpi) làm EtherCAT master cho driver bước Leadshine DM3E-556,
có web UI cổng 8080. Xem README.md cho build/chạy.

## Ràng buộc máy

- Pi đang chạy music server: Docker Lyrion (9000, 9090, 3483), Navidrome (4533),
  squeezelite, shairport-sync (7000), tailscale, một uvicorn khác ở 8090.
  Không được ảnh hưởng các dịch vụ này, không dùng trùng cổng.
- `eth0` chỉ dành cho EtherCAT, đặt unmanaged trong NetworkManager
  (`/etc/NetworkManager/conf.d/99-ethercat.conf`). Không IP, IPv6 tắt.
- Hỏi người dùng trước khi: cài gói apt, sửa /boot, thay kernel, tạo dịch vụ systemd.
- Mỗi bước chạy được thì commit và push.

## Quyết định

- **SOEM v2.0.1** (submodule `third_party/SOEM`), không dùng IgH: user-space,
  không cần module kernel (bcmgenet không có driver native IgH), không phải build
  lại khi đổi kernel. API v2 chỉ có `ecx_*` với `ecx_contextt`. GPLv3.
- **RT bằng C, web bằng Python/FastAPI** (venv, chỉ pip). Hai tiến trình tách biệt:
  trạng thái qua POSIX shm (seqlock, RT ghi, web đọc), lệnh qua Unix datagram
  socket (RT đọc non-blocking mỗi chu kỳ). RT không bao giờ chờ web.
- **Jog dùng CSP (mode 8)** với profile vận tốc hình thang sinh ở master.
  DM3E không hỗ trợ CSV: 0x6502 = 0xA5 → chỉ PP(1), PV(3), HM(6), CSP(8).
- Heartbeat 200 ms kiểm tra **trong tiến trình RT**. Giới hạn v/a từ
  `config/axis.ini` cũng áp trong RT.
- Khởi động hoặc mất bus: trục luôn disable, không tự enable.
- Kernel: hiện là PREEMPT (không RT). Kế hoạch: đo jitter trước với SCHED_FIFO,
  mlockall, isolcpus=3, IRQ eth0 → CPU3, Docker cpuset 0-2; chỉ đổi PREEMPT_RT
  nếu jitter xấu nhất > ~200 µs. Chưa sửa /boot.

## Đo thực tế

- 2026-11-04, kernel PREEMPT, không isolcpus, thread RT ghim CPU3, FIFO 80,
  music server chạy: 121k chu kỳ 1 ms, latency đánh thức max 188 µs,
  >100 µs: 14 lần, 0 lỗi WKC. Frame gửi lệch nửa chu kỳ (500 µs) so với
  Sync0, nên chưa cần PREEMPT_RT.

## DM3E-556 (đo thực tế, FW driver 301, stack 111)

- Vendor 0x4321, product 0x8100, rev 1. Có DC. Vòng hở (không encoder):
  6064h chỉ phản ánh lệnh.
- PDO mặc định: RxPDO 0x1600 = 6040, 607A, 60B8 (8 byte).
  TxPDO 0x1A00 = 603F, 6041, 6061, 6064, 60B9, 60BA, 60FD (19 byte).
  Không có 606C trong TxPDO.
- 0x2001 xung/vòng = 50000; 0x2000 dòng đỉnh = 4000 mA; 0x6080 = 3000 rpm.
- Motor D57CM31X (57, 1.8°, 5 A, không phanh, trục tự do). Nguồn hiện chỉ
  24 V / 1.5 A → giữ dòng đỉnh và tốc độ jog thấp, cẩn thận sụt áp (0x3221)
  khi tăng tốc.
- Không có ESI cho DM3E; ESI EM3E (cùng hãng/stack) dùng tham khảo: DC
  AssignActivate 0x0300 (chỉ Sync0), SM2 0x64. Không commit file ESI hãng.
- Vận tốc thực tế hiển thị: tính từ chênh lệch 6064 giữa các chu kỳ
  (không remap PDO thêm 606C).
- Bảng mã lỗi 603Fh: `DM3E/DM3E_UM_vi_V2.07.md` chương 5.
