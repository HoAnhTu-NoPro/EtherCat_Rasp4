"""Đăng nhập bằng một mật khẩu chung (hash scrypt trong config/web.ini) + session cookie."""

import hashlib
import hmac
import secrets
import time

COOKIE = "ecat_session"
MAX_FAILS = 5
FAIL_WINDOW_S = 300


def hash_password(pw: str) -> str:
    salt = secrets.token_bytes(16)
    n, r, p = 2**14, 8, 1
    h = hashlib.scrypt(pw.encode(), salt=salt, n=n, r=r, p=p, dklen=32)
    return f"scrypt${n}${r}${p}${salt.hex()}${h.hex()}"


def verify_password(pw: str, stored: str) -> bool:
    try:
        algo, n, r, p, salt, h = stored.split("$")
        if algo != "scrypt":
            return False
        calc = hashlib.scrypt(pw.encode(), salt=bytes.fromhex(salt), n=int(n), r=int(r), p=int(p),
                              dklen=len(h) // 2)
        return hmac.compare_digest(calc.hex(), h)
    except ValueError:
        return False


class Sessions:
    def __init__(self, password_hash: str, lifetime_s: int):
        self.password_hash = password_hash
        self.lifetime_s = lifetime_s
        self._tokens: dict[str, float] = {}
        self._fails: dict[str, list[float]] = {}

    def locked(self, ip: str) -> bool:
        now = time.monotonic()
        recent = [t for t in self._fails.get(ip, []) if now - t < FAIL_WINDOW_S]
        self._fails[ip] = recent
        return len(recent) >= MAX_FAILS

    def login(self, ip: str, pw: str) -> str | None:
        if verify_password(pw, self.password_hash):
            self._fails.pop(ip, None)
            token = secrets.token_urlsafe(32)
            self._tokens[token] = time.monotonic() + self.lifetime_s
            return token
        self._fails.setdefault(ip, []).append(time.monotonic())
        return None

    def valid(self, token: str | None) -> bool:
        if not token:
            return False
        exp = self._tokens.get(token)
        if exp is None:
            return False
        if time.monotonic() > exp:
            del self._tokens[token]
            return False
        return True

    def logout(self, token: str | None):
        self._tokens.pop(token or "", None)
