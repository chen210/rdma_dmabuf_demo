CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?=
LDLIBS ?= -libverbs -ldl

TARGET := rdma_dmabuf_mr_demo
SRC := rdma_dmabuf_mr_demo.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)
