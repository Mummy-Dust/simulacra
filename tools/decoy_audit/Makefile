CC ?= cc
ROOT := ../..
SRC := synth_dump.c ble_hs_adv.c roster_stub.c \
       $(ROOT)/main/generate.c $(ROOT)/main/templates.c $(ROOT)/main/roster.c $(ROOT)/main/ble_devices.c \
       $(ROOT)/main/learn.c $(ROOT)/components/simulacra_radar/law3.c $(ROOT)/components/simulacra_radar/learn_wire.c \
       $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c $(ROOT)/main/probe_agents.c $(ROOT)/main/probe_frame.c \
       $(ROOT)/main/fleet_pop.c $(ROOT)/main/fleet.c
INC := -Ihost_stubs -I$(ROOT)/main -I$(ROOT)/components/simulacra_radar
CFLAGS ?= -O2 -Wall -Wno-unused-parameter
all: synth_dump
synth_dump: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o synth_dump
clean:
	rm -f synth_dump synth_dump.exe
