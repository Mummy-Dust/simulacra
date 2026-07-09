CC ?= cc
ROOT := ../..
SRC := harness.c \
       $(ROOT)/components/simulacra_radar/law3.c \
       $(ROOT)/components/simulacra_radar/learn_wire.c \
       $(ROOT)/main/learn.c
SCAN_SRC := sig_scan.c \
       $(ROOT)/components/simulacra_radar/sig_match.c \
       $(ROOT)/components/simulacra_radar/sig_seed.c
INC := -Ihost_stubs -I$(ROOT)/components/simulacra_radar -I$(ROOT)/main
CFLAGS ?= -O2 -Wall -Wno-unused-parameter

all: harness sig_scan

harness: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o harness

sig_scan: $(SCAN_SRC)
	$(CC) $(CFLAGS) $(INC) $(SCAN_SRC) -o sig_scan

clean:
	rm -f harness harness.exe sig_scan sig_scan.exe
