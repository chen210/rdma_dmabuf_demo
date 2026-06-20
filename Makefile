CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?=
LDLIBS ?= -libverbs -ldl

TARGETS := rdma_dmabuf_mr_demo rdma_dmabuf_p2p_test

.PHONY: all clean

all: $(TARGETS)

rdma_dmabuf_mr_demo: rdma_dmabuf_mr_demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

rdma_dmabuf_p2p_test: rdma_dmabuf_p2p_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGETS)
