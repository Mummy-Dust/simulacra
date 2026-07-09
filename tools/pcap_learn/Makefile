CC ?= cc
ROOT := ../..
SRC := harness.c \
       $(ROOT)/components/simulacra_radar/law3.c \
       $(ROOT)/components/simulacra_radar/learn_wire.c \
       $(ROOT)/main/learn.c
INC := -Ihost_stubs -I$(ROOT)/components/simulacra_radar -I$(ROOT)/main
CFLAGS ?= -O2 -Wall -Wno-unused-parameter

harness: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o harness

clean:
	rm -f harness harness.exe
