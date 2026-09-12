CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

BIN := two_stream_recovery
OBJS := main.o input_udp.o output_udp.o report_stats.o ts_packet.o recovery_engine.o

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(BIN) $(OBJS)
