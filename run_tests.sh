#!/usr/bin/env bash
set -u

DEVICE="mlx5_0"
LENGTH="4096"
DRM_NODE="auto"
AMDGPU_DOMAIN="vram"
TRY_GTT=1
SKIP_BUILD=0
SKIP_AMDGPU=0
REQUIRE_AMDGPU=0
SUDO_AMDGPU=0

FAILURES=0
WARNINGS=0
SKIPS=0

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

usage()
{
	cat <<EOF
Usage:
  $0 [options]

Options:
  --device NAME          RDMA device name. Default: mlx5_0
  --length BYTES         MR length. Default: 4096
  --drm-node PATH        DRM render node, or "auto", or "none". Default: auto
  --amdgpu-domain NAME   vram, gtt, cpu, or numeric domain. Default: vram
  --no-try-gtt           Do not retry with --amdgpu-domain gtt after vram fails
  --skip-build           Do not run make
  --skip-amdgpu          Only test normal MR path
  --require-amdgpu       Treat AMDGPU exporter skip/failure as test failure
  --sudo-amdgpu          Run only the AMDGPU exporter test through sudo
  -h, --help             Show this help

Examples:
  $0
  $0 --device mlx5_0 --length 4096
  $0 --drm-node /dev/dri/renderD128 --amdgpu-domain vram
  $0 --sudo-amdgpu --drm-node /dev/dri/renderD128
EOF
}

section()
{
	printf '\n== %s ==\n' "$1"
}

note()
{
	printf '[INFO] %s\n' "$1"
}

warn()
{
	WARNINGS=$((WARNINGS + 1))
	printf '[WARN] %s\n' "$1"
}

skip()
{
	SKIPS=$((SKIPS + 1))
	printf '[SKIP] %s\n' "$1"
	if [ "$REQUIRE_AMDGPU" -eq 1 ]; then
		FAILURES=$((FAILURES + 1))
	fi
}

fail()
{
	FAILURES=$((FAILURES + 1))
	printf '[FAIL] %s\n' "$1"
}

run()
{
	printf '+'
	printf ' %q' "$@"
	printf '\n'
	"$@"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--device)
		DEVICE="${2:-}"
		shift 2
		;;
	--length)
		LENGTH="${2:-}"
		shift 2
		;;
	--drm-node)
		DRM_NODE="${2:-}"
		shift 2
		;;
	--amdgpu-domain)
		AMDGPU_DOMAIN="${2:-}"
		shift 2
		;;
	--no-try-gtt)
		TRY_GTT=0
		shift
		;;
	--skip-build)
		SKIP_BUILD=1
		shift
		;;
	--skip-amdgpu)
		SKIP_AMDGPU=1
		shift
		;;
	--require-amdgpu)
		REQUIRE_AMDGPU=1
		shift
		;;
	--sudo-amdgpu)
		SUDO_AMDGPU=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'Unknown option: %s\n\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
done

if [ -z "$DEVICE" ] || [ -z "$LENGTH" ]; then
	usage >&2
	exit 2
fi

section "Configuration"
note "repo=$SCRIPT_DIR"
note "device=$DEVICE"
note "length=$LENGTH"
note "drm_node=$DRM_NODE"
note "amdgpu_domain=$AMDGPU_DOMAIN"

section "Environment"
if command -v ibv_devinfo >/dev/null 2>&1; then
	run ibv_devinfo -d "$DEVICE" || warn "ibv_devinfo failed for $DEVICE"
else
	warn "ibv_devinfo not found"
fi

if [ -e /usr/include/infiniband/verbs.h ]; then
	note "found /usr/include/infiniband/verbs.h"
else
	fail "missing /usr/include/infiniband/verbs.h; install libibverbs-dev"
fi

if [ -e /usr/include/drm/amdgpu_drm.h ]; then
	note "found /usr/include/drm/amdgpu_drm.h"
else
	warn "missing /usr/include/drm/amdgpu_drm.h; AMDGPU exporter build may fail"
fi

section "Build"
if [ "$SKIP_BUILD" -eq 0 ]; then
	run make || fail "make failed"
else
	note "build skipped"
fi

if [ ! -x ./rdma_dmabuf_mr_demo ]; then
	fail "missing executable ./rdma_dmabuf_mr_demo"
else
	if ./rdma_dmabuf_mr_demo --help >/dev/null 2>&1; then
		note "--help passed"
	else
		fail "--help failed"
	fi
fi

section "Normal MR"
if [ -x ./rdma_dmabuf_mr_demo ]; then
	if run ./rdma_dmabuf_mr_demo --device "$DEVICE" --normal --length "$LENGTH"; then
		note "normal MR registration passed"
	else
		fail "normal MR registration failed"
	fi
fi

find_render_node()
{
	local n

	for n in /dev/dri/renderD*; do
		if [ -e "$n" ]; then
			printf '%s\n' "$n"
			return 0
		fi
	done

	return 1
}

get_drm_driver()
{
	local node="$1"
	local base

	base="$(basename "$node")"
	if [ -e "/sys/class/drm/$base/device/driver" ]; then
		basename "$(readlink -f "/sys/class/drm/$base/device/driver")"
	else
		return 1
	fi
}

run_amdgpu_test()
{
	local node="$1"
	local domain="$2"
	local cmd=(./rdma_dmabuf_mr_demo --device "$DEVICE" --amdgpu-drm "$node" --length "$LENGTH" --amdgpu-domain "$domain")

	if [ "$SUDO_AMDGPU" -eq 1 ]; then
		cmd=(sudo "${cmd[@]}")
	fi

	run "${cmd[@]}"
}

section "AMDGPU dma-buf exporter"
if [ "$SKIP_AMDGPU" -eq 1 ] || [ "$DRM_NODE" = "none" ]; then
	skip "AMDGPU exporter test disabled"
else
	if [ "$DRM_NODE" = "auto" ]; then
		if ! DRM_NODE="$(find_render_node)"; then
			skip "no /dev/dri/renderD* node found"
			DRM_NODE=""
		fi
	fi

	if [ -n "$DRM_NODE" ]; then
		if [ ! -e "$DRM_NODE" ]; then
			skip "DRM node does not exist: $DRM_NODE"
		else
			DRM_DRIVER="$(get_drm_driver "$DRM_NODE" || true)"
			if [ -n "$DRM_DRIVER" ]; then
				note "$DRM_NODE driver=$DRM_DRIVER"
			else
				warn "cannot find sysfs driver for $DRM_NODE"
			fi

			if [ -n "$DRM_DRIVER" ] && [ "$DRM_DRIVER" != "amdgpu" ]; then
				skip "$DRM_NODE uses driver=$DRM_DRIVER, not amdgpu"
			elif run_amdgpu_test "$DRM_NODE" "$AMDGPU_DOMAIN"; then
				note "AMDGPU exporter dmabuf MR registration passed with domain=$AMDGPU_DOMAIN"
			else
				if [ "$AMDGPU_DOMAIN" != "gtt" ] && [ "$TRY_GTT" -eq 1 ]; then
					warn "AMDGPU exporter failed with domain=$AMDGPU_DOMAIN; retrying domain=gtt"
					if run_amdgpu_test "$DRM_NODE" "gtt"; then
						note "AMDGPU exporter passed with domain=gtt"
					else
						if [ "$REQUIRE_AMDGPU" -eq 1 ]; then
							fail "AMDGPU exporter failed with both $AMDGPU_DOMAIN and gtt"
						else
							warn "AMDGPU exporter did not pass; see messages above"
						fi
					fi
				else
					if [ "$REQUIRE_AMDGPU" -eq 1 ]; then
						fail "AMDGPU exporter failed"
					else
						warn "AMDGPU exporter did not pass; see messages above"
					fi
				fi
			fi
		fi
	fi
fi

section "Summary"
note "failures=$FAILURES warnings=$WARNINGS skips=$SKIPS"

if [ "$FAILURES" -ne 0 ]; then
	exit 1
fi

exit 0
