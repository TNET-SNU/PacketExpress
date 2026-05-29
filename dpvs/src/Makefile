#
# DPVS is a software load balancer (Virtual Server) based on DPDK.
#
# Copyright (C) 2021 iQIYI (www.iqiyi.com).
# All Rights Reserved.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#

#
# Makefile for dpvs (DPVS main program).
#

TARGET := dpvs

ifneq ("$(wildcard VERSION)","")
    VERSION_STRING := $(shell ./VERSION)
else
    VERSION_STRING := $(shell git describe --tags --always)
endif

DATE_STRING := $(shell date +%Y.%m.%d.%H:%M:%S)

# same path of THIS Makefile
SRCDIR := $(dir $(realpath $(firstword $(MAKEFILE_LIST))))

# Addtional libs below are needed when using dynamic link.
LIBS += -lpthread -lnuma -lrt -lm -ldl -lcrypto

ifeq ($(shell pkg-config --exists libssl && echo 0),0)
CFLAGS += $(shell pkg-config --cflags libssl)
LIBS += $(shell pkg-config --static --libs libssl)
endif

CFLAGS += $(shell pkg-config --cflags libdpdk)
LIBS += $(shell pkg-config --static --libs libdpdk)

include $(SRCDIR)/config.mk

INCDIRS += -I $(SRCDIR)/../include

# for dpvs main program.
CFLAGS += -D __DPVS__ -DDPVS_VERSION=\"$(VERSION_STRING)\" -DDPVS_BUILD_DATE=\"$(DATE_STRING)\"
CFLAGS += -Wall -Werror -Wstrict-prototypes -Wmissing-prototypes -mcmodel=medium \
		-Wno-error=deprecated-declarations -Wno-error=address-of-packed-member

################################################################################

# Choose one of below

# 1. Using 1500B MTU to 9000B MTU translation
# CFLAGS += -D JUMBO_FRAME=0

# 2. Using 9000B MTU by default
CFLAGS += -D JUMBO_FRAME=1

################################################################################

# Choose one of below

# 1. Semantic test on (split uplink flows and merge downlink flows)
# CFLAGS += -D SEMANTIC_TEST=1

# 2. Semantic test off (merge all flows)
CFLAGS += -D SEMANTIC_TEST=0

################################################################################

# Choose one of below

# 1. DPDK GRO on
# CFLAGS += -D USE_GRO=1

# 2. DPDK GRO off
CFLAGS += -D USE_GRO=0

################################################################################

# Choose one of below

# 1. LRO on
CFLAGS += -D USE_LRO=1

# 2. LRO off
# CFLAGS += -D USE_LRO=0

################################################################################

# Choose one of below

# 1. PX on
CFLAGS += -D USE_PX=1

# 2. PX off
# CFLAGS += -D USE_PX=0

################################################################################

# Choose one of below

# 1. Dynamic RXQ scaling on
CFLAGS += -D USE_DYNRXQ=1

# 2. Dynamic RXQ scaling off
# CFLAGS += -D USE_DYNRXQ=0

################################################################################

# Choose one of below

# 1. delayed-merging off
CFLAGS += -D CUTOFF_NS=0

# 2. delayed-merging on
# CFLAGS += -D CUTOFF_NS=72000

################################################################################

# Choose one of below

# 1. NICMEM on (To use nicmem, make sure to use DPDK version 20.11)
# CFLAGS += -D NICMEM=1

# 2. NICMEM off
CFLAGS += -D NICMEM=0

################################################################################

# Choose one of below

# 1. Linking on
# CFLAGS += -D USE_LINK=1

# 2. Linking off
CFLAGS += -D USE_LINK=0

################################################################################

# Choose one of below

# 1. Hairpin off
CFLAGS += -D NUM_HAIRPIN_Q=0 -D HAIRPIN_POLICY=0

# 2. Hairpin on (1 queue, opt-out mode)
# CFLAGS += -D NUM_HAIRPIN_Q=1 -D HAIRPIN_POLICY=1

# 3. Hairpin on (1 queue, opt-in mode)
# CFLAGS += -D NUM_HAIRPIN_Q=1 -D HAIRPIN_POLICY=2

################################################################################

ifeq ($(shell test $(GCC_VERSION) -ge 70 && echo 1), 1)
	CFLAGS += -Wno-format-truncation
	CFLAGS += -Wno-stringop-truncation
	CFLAGS += -Wstringop-overflow=0
endif

ifneq ($(CONFIG_DEBUG), y)
	CFLAGS += -g -O3
else
	CFLAGS += -g -O0 -D DEBUG
	CFLAGS += -rdynamic
endif


CFLAGS += $(INCDIRS)

OBJS := $(shell find $(SRCDIR) -name '*.c' | sort)
OBJS := $(patsubst %.c,%.o,$(OBJS))

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  $(notdir $@)"
	$(Q)$(CC) $(CFLAGS) $^ $(LIBS) -o $@

%.o: %.c
	@echo "  $(notdir $@)"
	$(Q)$(CC) -c $(CFLAGS) $< -o $@

clean:
	find $(SRCDIR) -name '*.o' | xargs rm -f
	rm -f ./$(TARGET)

install:
	install -m 744 $(TARGET) $(INSDIR)
