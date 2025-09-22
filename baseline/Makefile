# pxgw DPDK-based gateway Makefile

APP     := gw
SRCS    := gw.c
OBJS    := $(SRCS:.c=.o)

CC      := gcc
CFLAGS  := -O3 -g -Wall -march=native -D_GNU_SOURCE -Wno-deprecated-declarations
PKGCONF := $(shell which pkg-config)
DPDK_CFLAGS := $(shell $(PKGCONF) --cflags libdpdk)
DPDK_LDFLAGS := $(shell $(PKGCONF) --libs libdpdk)

################################################################################

# Choose one of below

# 1. Using 1500B MTU for incoming packets
CFLAGS += -D JUMBO_FRAME=0

# 2. Using 9000B MTU for incoming packets
# CFLAGS += -D JUMBO_FRAME=1

################################################################################

# Choose one of below

# 1. HW checksum on
# CFLAGS += -D HW_CSUM=1

# 2. HW checksum off
CFLAGS += -D HW_CSUM=0

################################################################################

# Choose one of below

# 1. LRO on
# CFLAGS += -D USE_LRO=1

# 2. LRO off
CFLAGS += -D USE_LRO=0

################################################################################

# Choose one of below

# 1. Using 1500B MTU for incoming packets
CFLAGS += -D USE_SGDMA=0

# 2. Using 9000B MTU for incoming packets
# CFLAGS += -D USE_SGDMA=1

################################################################################

# Choose one of below

# 1. Semantic test on
# CFLAGS += -D SEMANTIC_TEST=1

# 2. Semantic test off
CFLAGS += -D SEMANTIC_TEST=0

################################################################################

all: $(APP)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(DPDK_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -c $< -o $@

clean:
	rm -f $(APP) $(OBJS)

.PHONY: all clean
