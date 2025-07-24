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
SRCS-$(CONFIG_RTE_LIBRTE_PX) += $(notdir $(wildcard $(SRCDIR)/*.c))

# install this header file
SYMLINK-$(CONFIG_RTE_LIBRTE_PX)-include += $(notdir $(wildcard $(SRCDIR)/*.h))

include $(RTE_SDK)/mk/rte.lib.mk
