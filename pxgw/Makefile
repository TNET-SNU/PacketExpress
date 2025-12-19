# pxgw DPDK-based gateway Makefile

APP     := pxgw
SRCS    := pxgw.c
OBJS    := $(SRCS:.c=.o)

CC      := gcc
CFLAGS  := -O3 -g -Wall -march=native -D_GNU_SOURCE -Wno-deprecated-declarations
# PKGCONF := $(shell which pkg-config)
# DPDK_CFLAGS := $(shell $(PKGCONF) --cflags libdpdk)
# DPDK_LDFLAGS := $(shell $(PKGCONF) --libs libdpdk)

DPDK_DIR := ../dpdk-stable-20.11.10-nicmem
DPDK_LIB := $(DPDK_DIR)/build/lib
DPDK_CFLAGS += -I$(DPDK_DIR)/build/include
DPDK_LDFLAGS += -L$(DPDK_LIB) -Wl,-rpath=$(DPDK_LIB) \
                -lrte_node -lrte_graph -lrte_bpf -lrte_flow_classify -lrte_pipeline \
                -lrte_table -lrte_port -lrte_fib -lrte_ipsec -lrte_vhost -lrte_stack \
                -lrte_security -lrte_sched -lrte_reorder -lrte_rib -lrte_regexdev \
                -lrte_rawdev -lrte_pdump -lrte_power -lrte_member -lrte_lpm \
                -lrte_latencystats -lrte_kni -lrte_jobstats -lrte_ip_frag -lrte_gso \
                -lrte_px -lrte_gro -lrte_eventdev -lrte_efd -lrte_distributor \
                -lrte_cryptodev -lrte_compressdev -lrte_cfgfile -lrte_bitratestats \
                -lrte_bbdev -lrte_acl -lrte_timer -lrte_hash -lrte_metrics -lrte_cmdline \
                -lrte_pci -lrte_ethdev -lrte_meter -lrte_net -lrte_mbuf -lrte_mempool \
                -lrte_rcu -lrte_ring -lrte_eal -lrte_telemetry -lrte_kvargs -lbsd


################################################################################

# Choose one of below

# 1. Using 1500B MTU for incoming packets
CFLAGS += -D JUMBO_FRAME=0

# 2. Using 9000B MTU for incoming packets
# CFLAGS += -D JUMBO_FRAME=1

################################################################################

# Choose one of below

# 1. Semantic test on (split uplink flows and merge downlink flows)
# CFLAGS += -D SEMANTIC_TEST=1

# 2. Semantic test off (merge all flows)
CFLAGS += -D SEMANTIC_TEST=0

################################################################################

# Choose one of below

# 1. LRO on
CFLAGS += -D USE_LRO=1

# 2. LRO off
# CFLAGS += -D USE_LRO=0

################################################################################

# Choose one of below

# 1. SGDMA on
CFLAGS += -D USE_SGDMA=1

# 2. SGDMA off
# CFLAGS += -D USE_SGDMA=0

################################################################################

# Choose one of below

# 1. Dynamic RXQ scaling on
CFLAGS += -D USE_DYNRXQ=1

# 2. Dynamic RXQ scaling off
# CFLAGS += -D USE_DYNRXQ=0

################################################################################

# Choose one of below

# 1. DPDK GRO on
# CFLAGS += -D USE_GRO=1

# 2. CUSTOM GRO on, no delay
# CFLAGS += -D USE_PX=1 -D CUTOFF_NS=0 -D USE_LINK=0

# 3. CUSTOM GRO on, delay
CFLAGS += -D USE_PX=1 -D CUTOFF_NS=120000 -D USE_LINK=0
# CFLAGS += -D USE_PX=1 -D CUTOFF_NS=800000 -D USE_LINK=0

# 4. CUSTOM GRO on, no delay, linking
# CFLAGS += -D USE_PX=1 -D CUTOFF_NS=0 -D USE_LINK=1

# 5. CUSTOM GRO on, delay, linking
# CFLAGS += -D USE_PX=1 -D CUTOFF_NS=120000 -D USE_LINK=1

################################################################################

# To use nicmem, make sure to use DPDK version 20.11
CFLAGS += -D NICMEM=0
# CFLAGS += -D NICMEM=1

################################################################################

# Choose one of below

# 1. Hairpin off
CFLAGS += -D NUM_HAIRPIN_Q=0 -D HAIRPIN_POLICY=0

# 2. Hairpin on (1 queue, opt-out mode)
# CFLAGS += -D NUM_HAIRPIN_Q=1 -D HAIRPIN_POLICY=1

# 3. Hairpin on (1 queue, opt-in mode)
# CFLAGS += -D NUM_HAIRPIN_Q=1 -D HAIRPIN_POLICY=2

################################################################################


all: $(APP)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(DPDK_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -c $< -o $@

clean:
	rm -f $(APP) $(OBJS)

.PHONY: all clean
