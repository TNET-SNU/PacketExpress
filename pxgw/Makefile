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
# CFLAGS += -D USE_DGRO=1

# 2. CUSTOM GRO on, no latent RXQ, no stall
# CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=1 -D CUTOFF_NS=0 -D USE_LINK=0

# 3. CUSTOM GRO on, no latent RXQ, stall
# CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=1 -D CUTOFF_NS=60000 -D USE_LINK=0

# 4. CUSTOM GRO on, latent RXQ 4, no stall
# CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=4 -D CUTOFF_NS=0 -D USE_LINK=0

# 5. CUSTOM GRO on, latent RXQ 4, stall
CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=4 -D CUTOFF_NS=60000 -D USE_LINK=0

# 6. CUSTOM GRO on, latent RXQ 4, no stall, linking
# CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=4 -D CUTOFF_NS=0 -D USE_LINK=1

# 7. CUSTOM GRO on, latent RXQ 4, stall, linking
# CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=4 -D CUTOFF_NS=60000 -D USE_LINK=1

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

# (not supported yet) if want to use rte_flow_async API, use below
# To use rte_flow_async API, make sure to use DPDK version 22.11

# CFLAGS += -D RTE_FLOW_ASYNC_API=1

# CFLAGS += -D RTE_FLOW_MEASURE_DELAY=1

################################################################################


all: $(APP)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(DPDK_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -c $< -o $@

clean:
	rm -f $(APP) $(OBJS)

.PHONY: all clean
