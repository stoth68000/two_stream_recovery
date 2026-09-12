CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

BIN := two_stream_recovery
OBJS := main.o command_line.o input_udp.o output_udp.o report_stats.o ts_packet.o recovery_engine.o
TEST_BIN := tests/test_recovery
TEST_OBJS := tests/test_recovery.o command_line.o input_udp.o output_udp.o report_stats.o ts_packet.o recovery_engine.o

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

clean:
	rm -f $(BIN) $(OBJS) $(TEST_BIN) $(TEST_OBJS)
