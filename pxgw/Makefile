# pxgw DPDK-based gateway Makefile

APP     := pxgw
SRCS    := pxgw.c
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

# 1. Semantic test on
# CFLAGS += -D SEMANTIC_TEST=1

# 2. Semantic test off
CFLAGS += -D SEMANTIC_TEST=0

################################################################################

# Choose one of below

# 1. LRO on
CFLAGS += -D USE_LRO=1

# 2. LRO off
# CFLAGS += -D USE_LRO=0

################################################################################

# Choose one of below

# 1. DPDK GRO on
# CFLAGS += -D USE_GRO=1

# 2. CUSTOM GRO on, no delay
# CFLAGS += -D USE_PX=1 -D CUTOFF_NS=0 -D USE_LINK=0

# 3. CUSTOM GRO on, delay
CFLAGS += -D USE_PX=1 -D CUTOFF_NS=120000 -D USE_LINK=0

# 4. CUSTOM GRO on, no delay, linking
# CFLAGS += -D USE_PX=1 -D CUTOFF_NS=0 -D USE_LINK=1

# 5. CUSTOM GRO on, delay, linking
# CFLAGS += -D USE_PX=1 -D CUTOFF_NS=120000 -D USE_LINK=1

################################################################################

# To use nicmem, make sure to use DPDK version 20.11
# CFLAGS += -D NICMEM=0
CFLAGS += -D NICMEM=1

################################################################################

# Choose one of below

# 1. Hairpin off
CFLAGS += -D NUM_HAIRPIN_Q=0

# 2. Hairpin on (1 queue)
# CFLAGS += -D NUM_HAIRPIN_Q=1

# 3. Hairpin on (1 queue, opt-out mode)
# CFLAGS += -D NUM_HAIRPIN_Q=1 -D USE_OPTOUT=1

################################################################################


all: $(APP)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(DPDK_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -c $< -o $@

clean:
	rm -f $(APP) $(OBJS)

.PHONY: all clean
