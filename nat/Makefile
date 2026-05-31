TARGET = nat

######################################################################
# GCC and compilation options
######################################################################
GCC = $(CC)
GCC_OPT = -m64 -Wall -Werror -fgnu89-inline
GCC_OPT += -DNDEBUG -O3 -g -DNETSTAT -DINFO -DDBGERR -DDBGCERR
GCC_OPT += $(DBG_OPT)
ifeq ($V,) # no echo
    export MSG=@echo
    export HIDE=@
else
    export MSG=@\#
    export HIDE=
endif

######################################################################
# LIBRARIES AND INCLUDES
######################################################################
MTCP_FLD    = ../mOS
MTCP_INC    = -I$(MTCP_FLD)/core/src/include
# MTCP_TARGET = ./libmtcp0.a
# MTCP_TARGET = ./libmtcp1.a
# MTCP_TARGET = ./libmtcp2.a
# MTCP_TARGET = ./libmtcp3.a
MTCP_TARGET = ./libmtcp4.a
LIBS        += -lssl -lcrypto -lnuma -lpthread -lrt
LIB_DIR     +=
CMN_DIR     = ./common
CMN_INC     = -I$(CMN_DIR)
UTIL_INC    = -I$(MTCP_FLD)/util/include

# DPDK HEADER
PKGCONF = pkg-config
DPDK_CFLAGS = $(shell $(PKGCONF) --cflags libdpdk)
DPDK_LDFLAGS += $(shell $(PKGCONF) --libs libdpdk)
INC += ${DPDK_CFLAGS}
LIBS += -pthread -lrt -lnuma -lpthread -libverbs -lm -lrt -ldl ${DPDK_LDFLAGS}
LIBS += -m64 -g -pthread -lrt -march=native -Wl,-export-dynamic -Wl,-lnuma -Wl,-lpthread -Wl,-lrt -Wl,-ldl

######################################################################

default: $(TARGET)

$(MTCP_TARGET):
	cd $(MTCP_FLD)/core/src && make	

$(TARGET): $(MTCP_TARGET) $(TARGET).c $(CMN_DIR)/*.c
	$(MSG) "   CC $<"
	$(HIDE) $(GCC) $(GCC_OPT) -o $@ $(TARGET).c $(CMN_DIR)/*.c $(DPDK_CFLAGS) $(MTCP_INC) $(CMN_INC) $(UTIL_INC) $(LIB_DIR) -Wl,--start-group $(MTCP_TARGET) -Wl,--end-group $(LIBS)

clean:
	rm -rf *~ *.o $(TARGET) logs/*