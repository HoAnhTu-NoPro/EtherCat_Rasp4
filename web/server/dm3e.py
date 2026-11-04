"""Giải mã trạng thái DM3E: statusword CiA 402, chế độ, mã lỗi 0x603F.

Bảng lỗi lấy từ DM3E/DM3E_UM_vi_V2.07.md chương 5.
"""

AXIS_STATES = [
    "Không rõ", "Not ready", "Switch on disabled", "Ready to switch on",
    "Switched on", "Enabled", "Quick stop", "Fault reaction", "Fault",
]

MODES = {0: "—", 1: "PP (vị trí profile)", 3: "PV (vận tốc profile)", 6: "Homing", 8: "CSP (vị trí đồng bộ)"}

STATUS_BITS = [
    (0, "Ready to switch on"), (1, "Switched on"), (2, "Operation enabled"), (3, "Fault"),
    (4, "Voltage enabled"), (5, "Quick stop"), (6, "Switch on disabled"), (7, "Warning"),
    (9, "Remote"), (10, "Target reached"), (11, "Internal limit"), (12, "Target ignored"),
]

ERRORS_603F = {
    0x2211: "Quá dòng",
    0x2212: "Quá dòng mô-đun IPM",
    0x3150: "Lỗi op-amp pha A",
    0x3151: "Lỗi op-amp pha B",
    0x3152: "Lỗi mạch đầu vào analog",
    0x3153: "Lỗi mạch đầu vào analog",
    0x3154: "Lỗi mạch đầu vào analog",
    0x3201: "Lỗi mạch bus DC",
    0x3205: "Điện áp điều khiển quá thấp",
    0x3211: "Điện áp bus DC quá cao",
    0x3221: "Điện áp bus DC quá thấp (kiểm tra nguồn 24 V)",
    0x4201: "Lỗi mạch phát hiện nhiệt độ",
    0x5201: "Enable ở chế độ không được hỗ trợ",
    0x5441: "Tín hiệu vào cảnh báo cưỡng bức",
    0x5510: "Tràn bộ nhớ",
    0x5530: "Lỗi lưu tham số EEPROM",
    0x5550: "Không truy cập được EEPROM",
    0x5551: "Lỗi EEPROM",
    0x5552: "Phần cứng chưa sẵn sàng",
    0x6321: "Lỗi phân bổ cổng vào I/F",
    0x6322: "Lỗi cài đặt chức năng cổng vào I/F",
    0x6323: "Lỗi cài đặt chức năng cổng ra I/F",
    0x7122: "Mất pha motor",
    0x7329: "Tín hiệu vượt hành trình dương/âm",
    0x7701: "Quá tải mạch xả điện trở",
    0x7702: "Lỗi điện trở xả",
    0x8201: "Chuyển trạng thái ESM thất bại",
    0x8207: "Đối tượng ánh xạ PDO không tồn tại",
    0x8208: "Độ dài đối tượng ánh xạ PDO sai",
    0x8209: "Đối tượng không có thuộc tính ánh xạ PDO",
    0x8210: "Đầu vào và đầu ra không hợp lệ",
    0x8211: "Không có dữ liệu đầu vào hợp lệ",
    0x8212: "Không có dữ liệu đầu ra hợp lệ",
    0x8213: "BOOT không được hỗ trợ",
    0x8215: "Cấu hình mailbox chế độ BOOT không hợp lệ",
    0x8216: "Cấu hình mailbox PRE-OP không hợp lệ",
    0x8217: "Cấu hình SM không hợp lệ",
    0x821B: "Quá thời gian watchdog SM2 (mất truyền thông)",
    0x821C: "Loại SM không hợp lệ",
    0x821D: "Cấu hình đầu ra không hợp lệ",
    0x821E: "Cấu hình đầu vào không hợp lệ",
    0x821F: "Cấu hình watchdog không hợp lệ",
    0x8224: "Ánh xạ TPDO không hợp lệ",
    0x8225: "Ánh xạ RPDO không hợp lệ",
    0x8311: "Quá tải motor",
    0x8401: "Rung động quá mức",
    0x8402: "Lệnh vượt quá tốc độ",
    0x8403: "Mất kiểm soát tốc độ / lượng tăng xung trong chu kỳ quá lớn",
    0x8503: "Cài đặt bánh răng điện tử lỗi",
    0x8612: "Tần số lệnh vị trí quá cao",
    0x871A: "Lỗi chế độ đồng bộ",
    0x8727: "Không hỗ trợ chế độ Free Run",
    0x8728: "Không hỗ trợ chế độ đồng bộ",
    0x872C: "Lỗi đồng bộ nghiêm trọng",
    0x872D: "Không có tín hiệu đồng bộ",
    0x872E: "Chu kỳ đồng bộ quá nhỏ",
    0x8730: "Cấu hình DC không hợp lệ",
    0x8732: "Lỗi DC PLL",
    0x8733: "Lỗi I/O đồng bộ DC",
    0x8734: "Quá thời gian đồng bộ DC",
    0x8735: "Chu kỳ DC không hợp lệ",
    0x8736: "Chu kỳ Sync0 không hợp lệ",
    0xA001: "Chuyển trạng thái ESM không hợp lệ (master đã reset bus)",
    0xA002: "Yêu cầu chuyển trạng thái ESM không xác định",
    0xA003: "Slave chờ yêu cầu INIT",
    0xA004: "Slave chờ yêu cầu PRE-OP",
    0xA005: "Slave chờ yêu cầu SAFE-OP",
    0xFF02: "Lỗi đồng bộ",
}

ESC_STATES = {1: "INIT", 2: "PREOP", 3: "BOOT", 4: "SAFEOP", 8: "OP"}


def error_text(code: int) -> str:
    if code == 0:
        return ""
    return ERRORS_603F.get(code, "Mã lỗi không có trong manual")


def esc_state_text(state: int) -> str:
    s = ESC_STATES.get(state & 0x0F, f"0x{state & 0x0F:X}")
    return s + (" + ERROR" if state & 0x10 else "")


def status_bits(sw: int) -> list[str]:
    return [name for bit, name in STATUS_BITS if sw & (1 << bit)]
