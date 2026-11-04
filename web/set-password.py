"""Đặt mật khẩu web: ghi hash scrypt vào config/web.ini (tạo từ web.example.ini nếu chưa có)."""

import configparser
import getpass
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server.auth import hash_password  # noqa: E402

root = Path(__file__).resolve().parents[1]
path = root / "config" / "web.ini"
if not path.exists():
    shutil.copy(root / "config" / "web.example.ini", path)

pw = getpass.getpass("Mật khẩu mới: ")
if len(pw) < 8:
    sys.exit("Mật khẩu phải có ít nhất 8 ký tự.")
if getpass.getpass("Nhập lại: ") != pw:
    sys.exit("Không khớp.")

cfg = configparser.ConfigParser()
cfg.read(path)
cfg.setdefault("web", {})
cfg["web"]["password_hash"] = hash_password(pw)
with open(path, "w") as f:
    cfg.write(f)
os.chmod(path, 0o600)
print(f"Đã lưu vào {path} (quyền 600). Khởi động lại web server để áp dụng.")
