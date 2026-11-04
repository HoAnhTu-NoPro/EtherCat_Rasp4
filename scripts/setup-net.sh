#!/bin/sh
# Chuẩn bị cổng EtherCAT: bật link, tắt IPv6 để kernel không gửi gói lạ lên bus.
# eth0 phải đã được đặt unmanaged trong NetworkManager (xem README).
set -e
IF="${1:-eth0}"
ip link set "$IF" up
sysctl -q -w "net.ipv6.conf.$IF.disable_ipv6=1"
ip -br link show "$IF"
