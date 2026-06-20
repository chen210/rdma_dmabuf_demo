#!/bin/sh
set -eu

TRACEFS=${TRACEFS:-/sys/kernel/tracing}
if [ ! -d "$TRACEFS/events" ]; then
	TRACEFS=/sys/kernel/debug/tracing
fi

if [ ! -d "$TRACEFS/events/dma_buf" ]; then
	echo "dma_buf trace events are not available under $TRACEFS" >&2
	exit 1
fi

need_event()
{
	if [ ! -e "$TRACEFS/events/dma_buf/$1/enable" ]; then
		echo "missing trace event: dma_buf:$1" >&2
		exit 1
	fi
}

need_event dma_buf_fd
need_event dma_buf_dynamic_attach
need_event dma_buf_detach

cleanup()
{
	echo 0 > "$TRACEFS/tracing_on" 2>/dev/null || true
	echo 0 > "$TRACEFS/events/dma_buf/dma_buf_fd/enable" 2>/dev/null || true
	echo 0 > "$TRACEFS/events/dma_buf/dma_buf_dynamic_attach/enable" 2>/dev/null || true
	echo 0 > "$TRACEFS/events/dma_buf/dma_buf_detach/enable" 2>/dev/null || true
}

trap cleanup EXIT INT TERM

echo 0 > "$TRACEFS/tracing_on"
: > "$TRACEFS/trace"
echo 1 > "$TRACEFS/events/dma_buf/dma_buf_fd/enable"
echo 1 > "$TRACEFS/events/dma_buf/dma_buf_dynamic_attach/enable"
echo 1 > "$TRACEFS/events/dma_buf/dma_buf_detach/enable"
echo 1 > "$TRACEFS/tracing_on"

cat >&2 <<'EOF'
Tracing dma_buf fd/attach events.

Start rdma_dmabuf_p2p_test in another terminal. Look for:
  dma_buf_fd:             exp_name=drm size=<length> ino=<debugfs ino> fd=<process fd>
  dma_buf_dynamic_attach: exp_name=drm size=<length> ino=<same ino> dev_name=<RNIC BDF>

Stop with Ctrl-C.
EOF

cat "$TRACEFS/trace_pipe"
