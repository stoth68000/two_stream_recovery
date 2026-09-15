CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

BIN := two_stream_recovery
OBJS := main.o command_line.o input_udp.o output_udp.o report_stats.o ts_packet.o recovery_engine.o web_server.o
TEST_BIN := tests/test_recovery
TEST_OBJS := tests/test_recovery.o command_line.o input_udp.o output_udp.o report_stats.o ts_packet.o recovery_engine.o web_server.o
SOAK_BIN := tests/soak_replay
SOAK_OBJS := tests/soak_replay.o report_stats.o ts_packet.o recovery_engine.o

.PHONY: all clean test live-test soak-test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

soak-test: $(SOAK_BIN)
	./$(SOAK_BIN)

$(TEST_BIN): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

$(SOAK_BIN): $(SOAK_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SOAK_OBJS) $(LDFLAGS)

clean:
	rm -f $(BIN) $(OBJS) $(TEST_BIN) $(TEST_OBJS) $(SOAK_BIN) $(SOAK_OBJS)

live-test: $(BIN)
	./two_stream_recovery \
		--input-primary-url udp://127.0.0.1:4501 \
		--input-secondary-url udp://127.0.0.1:4502 \
		--output-url udp://127.0.0.1:4503 \
		--primary-delay-ms 4000 \
		--alignment-window-ms 6000 \
		--history-ms 10000 \
		--max-content-burst-packets 255 \
		--http-port 4500 \
		--console-report
