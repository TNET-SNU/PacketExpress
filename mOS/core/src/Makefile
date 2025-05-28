
.PHONY: clean

SHELL := /bin/bash
DISABLE_NUMA=@NONUMA@

### TARGET ###
MTCP_LIB_DIR=../lib
MTCP_LIB=libmtcp.a

MTCP_HDR_DIR=../include
MTCP_HDR = mtcp_api.h mos_api.h mtcp_epoll.h mtcp_util.h


### GCC ###
GCC = gcc
GCC_OPT = -m64 -Wall -fPIC -DNEWEV -fgnu89-inline -Werror -fcommon -Wno-error=deprecated-declarations

#DBG_OPT = -DDBGMSG -DDBGFUNC -DSTREAM -DSTATE -DTSTAT -DAPP -DEPOLL
#DBG_OPT = -DDBGMSG -DDBGFUNC -DSTREAM -DSTATE
#DBG_OPT += -DPKTDUMP
#DBG_OPT += -DDUMP_STREAM
#GCC_OPT += -g -DNETSTAT -DINFO -DDBGERR -DDBGCERR
GCC_OPT += -DNDEBUG -O3 -g -DNETSTAT -DINFO -DDBGERR -DDBGCERR
# GCC_OPT += -DNDEBUG -O0 -g -DNETSTAT -DINFO -DDBGERR -DDBGCERR
GCC_OPT += $(DBG_OPT)

ifeq ($V,) # no echo
    export MSG=@echo
    export HIDE=@
else
    export MSG=@\#
    export HIDE=
endif

### LIBRARIES AND INCLUDES ###
INC_DIR=./include
INC= -I$(INC_DIR) -I$(INC_DIR)/bpf

ifeq ($(shell uname), Darwin)
GCC_OPT += -DDARWIN -Wno-deprecated -Dinline=''
endif

LIBS = -lpthread -lrt
ifeq ($(DISABLE_NUMA), 1)
GCC_OPT += -DDISABLE_NUMA
LIBS += -lnuma
endif

PKGCONF = pkg-config
DPDK_CFLAGS = $(shell $(PKGCONF) --cflags libdpdk)
DPDK_LDFLAGS += $(shell $(PKGCONF) --libs libdpdk)
INC += $(DPDK_CFLAGS)
LDFLAGS += $(DPDK_LDFLAGS)
LIBS += -export-dynamic

### SOURCE CODE ###
SRCS = core.c tcp_stream.c config.c api.c mos_api.c eventpoll.c socket.c pipe.c \
	   tcp_util.c eth_in.c ip_in.c tcp.c tcp_in.c eth_out.c ip_out.c tcp_out.c \
	   arp.c timer.c cpu.c util.c addr_pool.c fhash.c memory_mgt.c logger.c debug.c \
	   tcp_rb_frag_queue.c tcp_send_buffer.c tcp_sb_queue.c tcp_stream_queue.c \
	bpf/sf_bpf_filter.c bpf/sfbpf-int.c bpf/sf_gencode.c bpf/sf_grammar.c \
	bpf/sf_nametoaddr.c bpf/sf_optimize.c bpf/sf_scanner.c \
	event_callback.c tcp_rb.c icmp.c scalable_event.c key_value_store.c dpdk_module.c

OBJS = $(patsubst %.c,%.o,$(SRCS))
DEPS = $(patsubst %.c,.%.d,$(SRCS))

################################################################################

# Choose one of below

# 1. Using 1500B MTU for incoming packets
CFLAGS += -D JUMBO_FRAME=0

# 2. Using 9000B MTU for incoming packets
# CFLAGS += -D JUMBO_FRAME=1

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
# CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=1 -D STALL_TIMEOUT_NS=0 -D USE_LINK=0

# 3. CUSTOM GRO on, no stall
# CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=4 -D STALL_TIMEOUT_NS=0 -D USE_LINK=0

# 4. CUSTOM GRO on, stall 50us
CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=4 -D STALL_TIMEOUT_NS=72000 -D USE_LINK=0

# 6. CUSTOM GRO + LINKING
# CFLAGS += -D USE_CGRO=1 -D NUM_LATENT_RXQ=4 -D STALL_TIMEOUT_NS=0 -D USE_LINK=1

################################################################################

# To use nicmem, make sure to use DPDK version 20.11
# CFLAGS += -D NICMEM=0
CFLAGS += -D NICMEM=1

################################################################################

# Choose one of below

CFLAGS += -D NUM_HAIRPIN_Q=0
# CFLAGS += -D NUM_HAIRPIN_Q=1

# (not supported yet) if want to use rte_flow_async API, use below
# To use rte_flow_async API, make sure to use DPDK version 22.11

# CFLAGS += -D RTE_FLOW_ASYNC_API=1

# CFLAGS += -D RTE_FLOW_MEASURE_DELAY=1

################################################################################


### GOALS ###
default: $(OBJS)
	mkdir -p $(MTCP_LIB_DIR)
	$(MSG) "   AR $(MTCP_LIB)"
	$(HIDE) ar rvs $(MTCP_LIB_DIR)/$(MTCP_LIB) $(OBJS)

$(OBJS): %.o: %.c Makefile calcCPUs
	$(MSG) "   CC $<"
	$(HIDE) $(GCC) $(GCC_OPT) $(INC) $(CFLAGS) -c $< -o $@
$(DEPS): .%.d: %.c Makefile
	@mkdir -p .bpf
	$(HIDE) $(GCC) $(GCC_OPT) $(INC) -MM $(CFLAGS) $< > $@

calcCPUs:
	$(eval export GCC_OPT := $(GCC_OPT) -DMAX_CPUS=$(shell let cpus=`lscpu | grep "Core(s) per socket" | awk '{print \$$4}'`*`lscpu | grep "Socket(s)" | awk '{print \$$2}'`; echo $$cpus))

-include $(DEPS)

clean: clean-library
	rm -f *.o *~ core
	rm -rf .*.d tags .bpf

clean-library:
	rm -f $(MTCP_LIB_DIR)/*

tags:
	find -name '*.c' -or -name '*.h' | xargs ctags
