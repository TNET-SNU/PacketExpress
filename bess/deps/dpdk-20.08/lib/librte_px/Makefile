# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Intel Corporation

include $(RTE_SDK)/mk/rte.vars.mk

# library name
LIB = librte_px.a

CFLAGS += -O3
CFLAGS += $(WERROR_FLAGS) -I$(SRCDIR)
LDLIBS += -lrte_eal -lrte_mbuf -lrte_ethdev -lrte_net -lrte_hash

EXPORT_MAP := rte_px_version.map

# source files
SRCS-$(CONFIG_RTE_LIBRTE_PX) += px_gro.c px_link.c px_ring.c px_rule.c px_stat.c

# install this header file
SYMLINK-$(CONFIG_RTE_LIBRTE_PX)-include += px_common.h px_gro.h px_ring.h px_link.h px_rule.h px_stat.h px_delay.h px_mss_trans.h

include $(RTE_SDK)/mk/rte.lib.mk
