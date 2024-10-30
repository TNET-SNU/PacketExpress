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
MTCP_FLD    = ../mOS/core
MTCP_INC    =-I$(MTCP_FLD)/src/include
MTCP_TARGET = $(MTCP_FLD)/lib/libmtcp.a
LIBS        += -lmtcp -lssl -lcrypto -lnuma -lpthread -lrt
LIB_DIR     += -L$(MTCP_FLD)/lib
CMN_DIR     = ./common
CMN_INC     = -I$(CMN_DIR)
UTIL_INC    = -I../mOS/util/include

# DPDK HEADER
PKGCONF = pkg-config
DPDK_CFLAGS = $(shell $(PKGCONF) --cflags libdpdk)
DPDK_LDFLAGS += $(shell $(PKGCONF) --libs libdpdk)
INC += ${DPDK_CFLAGS}
LIBS += -pthread -lrt ${MTCP_FLD}/lib/libmtcp.a -lnuma -lmtcp -lpthread -libverbs -lm -lrt -ldl ${DPDK_LDFLAGS}

# I/O library parameter (PSIO or DPDK)
LIBS    += -m64 -g -pthread -lrt -march=native -Wl,-export-dynamic -Wl,-lnuma -Wl,-lmtcp -Wl,-lpthread -Wl,-lrt -Wl,-ldl

######################################################################

default: $(TARGET)

$(MTCP_TARGET):
	cd $(MTCP_FLD)/src && make	

$(TARGET): $(MTCP_TARGET) $(TARGET).c $(CMN_DIR)/*.c
	$(MSG) "   CC $<"
	$(HIDE) $(GCC) $(GCC_OPT) -o $@ $^ $(DPDK_CFLAGS) $(DPDK_LDFLAGS) $(MTCP_INC) $(CMN_INC) $(UTIL_INC) $(LIB_DIR) $(LIBS) 

clean:
	rm -rf *~ *.o $(TARGET) logs/*

cleanall: clean
	rm -rf Makefile


