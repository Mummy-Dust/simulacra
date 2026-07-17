CC ?= cc
ROOT := ../..
SRC := probe_dump.c $(ROOT)/main/probe_frame.c $(ROOT)/main/probe_agents.c $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c
INC := -Ihost_stubs -I$(ROOT)/main
CFLAGS ?= -O2 -Wall -Wno-unused-parameter
all: probe_dump
probe_dump: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o probe_dump
clean:
	rm -f probe_dump probe_dump.exe
