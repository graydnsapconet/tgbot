CC      := clang
SRC_DIR := ../src
LIB_DIR := ../lib
BUILD   := build

FUZZ_SECONDS ?= 60

CFLAGS  := -Wall -Wextra -pedantic -std=c11 -O1 -g3       \
           -fno-omit-frame-pointer                          \
           -fsanitize=fuzzer,address,undefined               \
           -pipe

CJSON_CFLAGS := -Wall -std=c11 -O1 -g3                     \
                -fno-omit-frame-pointer                     \
                -fsanitize=fuzzer,address,undefined           \
                -pipe

LDFLAGS := -lpthread -fsanitize=fuzzer,address,undefined

# ── Objects ──────────────────────────────────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/cJSON.o: $(LIB_DIR)/cJSON.c | $(BUILD)
	$(CC) $(CJSON_CFLAGS) -I$(LIB_DIR) -c $< -o $@

$(BUILD)/fuzz_webhook.o: fuzz_webhook.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/fuzz_commands.o: fuzz_commands.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

# ── Fuzz binaries ────────────────────────────────────────────────────
$(BUILD)/fuzz_webhook: $(BUILD)/fuzz_webhook.o $(BUILD)/commands.o $(BUILD)/queue.o $(BUILD)/whitelist.o $(BUILD)/logger.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/fuzz_commands: $(BUILD)/fuzz_commands.o $(BUILD)/commands.o $(BUILD)/queue.o $(BUILD)/whitelist.o $(BUILD)/logger.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

FUZZ_TARGETS := $(BUILD)/fuzz_webhook $(BUILD)/fuzz_commands

.PHONY: all clean run

all: $(FUZZ_TARGETS)

run: $(FUZZ_TARGETS)
	@echo "=== Fuzzing fuzz_webhook for $(FUZZ_SECONDS)s ==="
	$(BUILD)/fuzz_webhook -max_total_time=$(FUZZ_SECONDS) corpus/
	@echo ""
	@echo "=== Fuzzing fuzz_commands for $(FUZZ_SECONDS)s ==="
	$(BUILD)/fuzz_commands -max_total_time=$(FUZZ_SECONDS) corpus/

clean:
	rm -rf $(BUILD)
