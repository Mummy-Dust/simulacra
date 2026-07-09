CC ?= cc
ROOT := ../..
SRC := synth_dump.c ble_hs_adv.c learn_stub.c \
       $(ROOT)/main/generate.c $(ROOT)/main/templates.c $(ROOT)/main/roster.c
INC := -Ihost_stubs -I$(ROOT)/main -I$(ROOT)/components/simulacra_radar
CFLAGS ?= -O2 -Wall -Wno-unused-parameter
all: synth_dump
synth_dump: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o synth_dump
clean:
	rm -f synth_dump synth_dump.exe
