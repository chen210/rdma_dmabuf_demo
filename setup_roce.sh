#!/bin/bash
#=============================================================================
# setup_roce.sh — 自动配置 Mellanox ConnectX-4 Lx 端口0 的 IP + RoCE v2 RDMA
#
# 硬件: Mellanox ConnectX-4 Lx (mlx5_0) → enp1s0f0np0
# 用法: sudo ./setup_roce.sh
#=============================================================================

set -euo pipefail

#──── 配置区 ──────────────────────────────────────────────────────────────────
IFACE="enp1s0f0np0"            # 网络接口名
IP_ADDR="192.168.100.1/24"     # 要配置的 IP/CIDR
MTU=9000                       # jumbo frame (RDMA 性能推荐)
RDMA_DEV="mlx5_0"              # 对应的 RDMA 设备名
RDMA_PORT=1
LINK_WAIT_SEC=10               # 等待链路 UP 的超时秒数
#──────────────────────────────────────────────────────────────────────────────

# 颜色输出
RED='\033[0;31m'; GRN='\033[0;32m'; YEL='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GRN}[ OK ]${NC}  $*"; }
warn()  { echo -e "${YEL}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERR]${NC}  $*" >&2; }

#────────── root 检查 ─────────────────────────────────────────────────────────
[[ $EUID -ne 0 ]] && { err "需要 root 权限，请用 sudo 运行"; exit 1; }

echo "========================================"
echo "  RoCE v2 自动配置  ($IFACE / $RDMA_DEV)"
echo "========================================"

#────────── 1. 加载 RDMA 内核模块 ──────────────────────────────────────────────
info "加载 RDMA 内核模块..."
MODULES=(mlx5_core mlx5_ib ib_core ib_uverbs rdma_ucm rdma_cm)
for mod in "${MODULES[@]}"; do
    if lsmod | grep -qw "$mod"; then
        ok "$mod 已加载"
    else
        modprobe "$mod" && ok "$mod 加载成功" || { err "$mod 加载失败"; exit 1; }
    fi
done

#────────── 2. 等待网卡设备出现 ────────────────────────────────────────────────
info "等待网卡设备 $IFACE 出现..."
for i in $(seq 1 "$LINK_WAIT_SEC"); do
    [[ -e "/sys/class/net/$IFACE" ]] && break
    sleep 1
done
[[ -e "/sys/class/net/$IFACE" ]] || { err "网卡 $IFACE 不存在"; exit 1; }
ok "设备 $IFACE 就绪"

#────────── 3. 配置 MTU ────────────────────────────────────────────────────────
info "设置 MTU=$MTU ..."
ip link set "$IFACE" mtu "$MTU"
CUR_MTU=$(cat "/sys/class/net/$IFACE/mtu")
ok "MTU = $CUR_MTU"

#────────── 4. 配置 IP 地址 ────────────────────────────────────────────────────
info "配置 IP 地址 $IP_ADDR ..."
# 先清除旧地址（避免重复）
ip addr flush dev "$IFACE"
ip addr add "$IP_ADDR" dev "$IFACE"
ok "IP 已设置: $IP_ADDR"

#────────── 5. 拉起网卡 ────────────────────────────────────────────────────────
info "启用接口 $IFACE ..."
ip link set "$IFACE" up

#────────── 6. 等待链路 UP ─────────────────────────────────────────────────────
info "等待链路 LINK_UP ..."
LINK_OK=0
for i in $(seq 1 "$LINK_WAIT_SEC"); do
    ST=$(cat "/sys/class/net/$IFACE/operstate" 2>/dev/null || echo "?")
    if [[ "$ST" == "up" ]]; then
        LINK_OK=1; break
    fi
    sleep 1
done
if [[ "$LINK_OK" -eq 1 ]]; then
    ok "链路状态: UP"
else
    warn "链路在 ${LINK_WAIT_SEC}s 内未 UP (当前: $ST)，请检查网线/对端"
fi

#────────── 7. 等待 RDMA link ACTIVE ───────────────────────────────────────────
info "等待 RDMA 设备 $RDMA_DEV 端口 $RDMA_PORT ACTIVE ..."
RDMA_OK=0
for i in $(seq 1 "$LINK_WAIT_SEC"); do
    RDMA_STATE=$(cat "/sys/class/infiniband/$RDMA_DEV/ports/$RDMA_PORT/state" 2>/dev/null | awk '{print $2}')
    if [[ "$RDMA_STATE" == "ACTIVE" ]]; then
        RDMA_OK=1; break
    fi
    sleep 1
done
if [[ "$RDMA_OK" -eq 1 ]]; then
    ok "RDMA 端口状态: ACTIVE"
else
    warn "RDMA 端口未 ACTIVE (当前: $RDMA_STATE)"
fi

#────────── 8. 验证汇总 ────────────────────────────────────────────────────────
echo ""
echo "========================================"
echo "           配置结果验证"
echo "========================================"

# IP
ACTUAL_IP=$(ip -o -4 addr show "$IFACE" 2>/dev/null | awk '{print $4}')
if [[ -n "$ACTUAL_IP" ]]; then
    ok "IP:  $ACTUAL_IP"
else
    err "IP:  未配置"
fi

# MTU
ok "MTU: $(cat /sys/class/net/$IFACE/mtu)"

# 网卡状态
ok "NIC: $(cat /sys/class/net/$IFACE/operstate)"

# RDMA link
RDMA_LINE=$(rdma link 2>/dev/null | grep "$RDMA_DEV" | head -1)
if [[ -n "$RDMA_LINE" ]]; then
    ok "RDMA: $RDMA_LINE"
else
    err "RDMA link 未找到"
fi

# RDMA 设备信息
if command -v ibv_devinfo &>/dev/null; then
    HCA_STATE=$(ibv_devinfo -d "$RDMA_DEV" 2>/dev/null | grep -m1 "state:" | awk '{print $2, $3}')
    ok "HCA:  $RDMA_DEV → $HCA_STATE"
fi

echo "========================================"
echo "  配置完成! "
echo "========================================"
