CC      := gcc

# use debug flags + sanitisers
CFLAGS  := -Wall -Wextra -pedantic -std=c11 -O0 -g3         \
           -Wformat=2 -Wshadow -Wconversion                 \
           -D_FORTIFY_SOURCE=2                              \
           -fno-omit-frame-pointer                          \
           -fstack-protector-all                            \
           -fsanitize=address,undefined                     \
           -pipe

LDFLAGS := -lpthread -lmicrohttpd -fsanitize=address,undefined

SRC_DIR := ../src
LIB_DIR := ../lib
BUILD   := build

# relax warnings for vendored libraries
CJSON_CFLAGS := -Wall -std=c11 -O0 -g3                      \
                -fno-omit-frame-pointer                     \
                -fsanitize=address,undefined                \
                -pipe

# each test binary links only the modules it needs.
QUEUE_OBJS   := $(BUILD)/queue.o
WEBHOOK_OBJS := $(BUILD)/webhook.o $(BUILD)/queue.o
WL_OBJS      := $(BUILD)/whitelist.o
CMD_OBJS     := $(BUILD)/commands.o $(BUILD)/queue.o $(BUILD)/whitelist.o
CFG_OBJS     := $(BUILD)/cfg.o
BOT_OBJS     := $(BUILD)/bot.o
LLM_OBJS     := $(BUILD)/llm.o
CJSON_OBJ    := $(BUILD)/cJSON.o
INI_OBJ      := $(BUILD)/ini.o
LOGGER_OBJ   := $(BUILD)/logger.o

# test bins
TESTS := $(BUILD)/test_queue $(BUILD)/test_webhook $(BUILD)/test_whitelist $(BUILD)/test_commands $(BUILD)/test_logger $(BUILD)/test_cfg $(BUILD)/test_stress $(BUILD)/test_bot $(BUILD)/test_llm

.PHONY: all clean test test-asan test-valgrind tsan

all: $(TESTS)

$(BUILD):
	mkdir -p $(BUILD)

# compile source modules
$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/cJSON.o: $(LIB_DIR)/cJSON.c | $(BUILD)
	$(CC) $(CJSON_CFLAGS) -I$(LIB_DIR) -c $< -o $@

$(BUILD)/ini.o: $(LIB_DIR)/ini.c | $(BUILD)
	$(CC) $(CJSON_CFLAGS) -I$(LIB_DIR) -c $< -o $@

# compile test files
$(BUILD)/test_queue.o: test_queue.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_webhook.o: test_webhook.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_whitelist.o: test_whitelist.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_commands.o: test_commands.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_logger.o: test_logger.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_cfg.o: test_cfg.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_stress.o: test_stress.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_bot.o: test_bot.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -DTESTING -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_llm.o: test_llm.c test.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/bot_test.o: $(SRC_DIR)/bot.c | $(BUILD)
	$(CC) $(CFLAGS) -DTESTING -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

# link test bins
$(BUILD)/test_queue: $(BUILD)/test_queue.o $(QUEUE_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/test_webhook: $(BUILD)/test_webhook.o $(WEBHOOK_OBJS) $(CJSON_OBJ) $(LOGGER_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/test_whitelist: $(BUILD)/test_whitelist.o $(WL_OBJS) $(LOGGER_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/test_commands: $(BUILD)/test_commands.o $(CMD_OBJS) $(LOGGER_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/test_logger: $(BUILD)/test_logger.o $(LOGGER_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/test_cfg: $(BUILD)/test_cfg.o $(CFG_OBJS) $(INI_OBJ) $(LOGGER_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/test_stress: $(BUILD)/test_stress.o $(QUEUE_OBJS) $(LOGGER_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/test_bot: $(BUILD)/test_bot.o $(BUILD)/bot_test.o $(CJSON_OBJ) $(LOGGER_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -lcurl -o $@

$(BUILD)/test_llm: $(BUILD)/test_llm.o $(LLM_OBJS) $(CJSON_OBJ) $(LOGGER_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -lcurl -o $@

# run all tests
# TODO: add text colour for PASSED and FAILED
test: $(TESTS)
	@echo ""
	@echo "======================================"
	@echo "    running tests ..."
	@echo "======================================"
	@fail=0;                                           \
	for t in $(TESTS); do                              \
	    echo "";                                       \
	    echo "--- $$t ---";                            \
	    if $$t; then                                   \
	        echo "  PASSED";                           \
	    else                                           \
	        echo "  FAILED";                           \
	        fail=1;                                    \
	    fi;                                            \
	done;                                              \
	echo "";                                           \
	if [ $$fail -eq 0 ]; then                          \
	    echo "======================================"; \
	    echo "  ALL test suites PASSED";               \
	    echo "======================================"; \
	else                                               \
	    echo "======================================"; \
	    echo "  some tests FAILED!";                   \
	    echo "======================================"; \
	    exit 1;                                        \
	fi

# alias
test-asan: test

# Valgrind run (for deeper stack/heap analysis)
VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=all \
                  --track-origins=yes --error-exitcode=1

# build without sanitisers for valgrind compatibility
comma := ,
VFLAGS := $(filter-out -fsanitize=address$(comma)undefined,$(CFLAGS))

# Valgrind-compatible object rules
$(BUILD)/%.vg.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(VFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/cJSON.vg.o: $(LIB_DIR)/cJSON.c | $(BUILD)
	$(CC) $(filter-out -fsanitize=address$(comma)undefined,$(CJSON_CFLAGS)) -I$(LIB_DIR) -c $< -o $@

$(BUILD)/ini.vg.o: $(LIB_DIR)/ini.c | $(BUILD)
	$(CC) $(filter-out -fsanitize=address$(comma)undefined,$(CJSON_CFLAGS)) -I$(LIB_DIR) -c $< -o $@

VLDFLAGS := $(filter-out -fsanitize=address$(comma)undefined,$(LDFLAGS))

$(BUILD)/test_queue.vg: $(BUILD)/test_queue.vg.o $(BUILD)/queue.vg.o | $(BUILD)
	$(CC) $(VFLAGS) $^ $(VLDFLAGS) -o $@

$(BUILD)/test_webhook.vg: $(BUILD)/test_webhook.vg.o $(BUILD)/webhook.vg.o $(BUILD)/queue.vg.o $(BUILD)/cJSON.vg.o $(BUILD)/logger.vg.o | $(BUILD)
	$(CC) $(VFLAGS) $^ $(VLDFLAGS) -o $@

$(BUILD)/test_whitelist.vg: $(BUILD)/test_whitelist.vg.o $(BUILD)/whitelist.vg.o $(BUILD)/logger.vg.o | $(BUILD)
	$(CC) $(VFLAGS) $^ $(VLDFLAGS) -o $@

$(BUILD)/test_commands.vg: $(BUILD)/test_commands.vg.o $(BUILD)/commands.vg.o $(BUILD)/queue.vg.o $(BUILD)/whitelist.vg.o $(BUILD)/logger.vg.o | $(BUILD)
	$(CC) $(VFLAGS) $^ $(VLDFLAGS) -o $@

$(BUILD)/test_logger.vg: $(BUILD)/test_logger.vg.o $(BUILD)/logger.vg.o | $(BUILD)
	$(CC) $(VFLAGS) $^ $(VLDFLAGS) -o $@

VG_TESTS := $(BUILD)/test_queue.vg $(BUILD)/test_webhook.vg $(BUILD)/test_whitelist.vg $(BUILD)/test_commands.vg $(BUILD)/test_logger.vg

# compile test source files for valgrind
$(BUILD)/test_%.vg.o: test_%.c test.h | $(BUILD)
	$(CC) $(VFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

test-valgrind: $(VG_TESTS)
	@echo ""
	@echo "======================================="
	@echo "    running tests under Valgrind ..."
	@echo "======================================="
	@for t in $(VG_TESTS); do                     \
	    echo "--- $$t ---";                       \
	    valgrind $(VALGRIND_FLAGS) $$t || exit 1; \
	done
	@echo "  All valgrind checks passed."

# TSan build
TSAN_CFLAGS := $(filter-out -fsanitize=address$(comma)undefined,$(CFLAGS))             \
               -fsanitize=thread -fPIE -pie
TSAN_LDFLAGS := $(filter-out -fsanitize=address$(comma)undefined,$(LDFLAGS))           \
                -fsanitize=thread -fPIE -pie
TSAN_CJSON_CFLAGS := $(filter-out -fsanitize=address$(comma)undefined,$(CJSON_CFLAGS)) \
                     -fsanitize=thread -fPIE -pie

$(BUILD)/%.tsan.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(TSAN_CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/cJSON.tsan.o: $(LIB_DIR)/cJSON.c | $(BUILD)
	$(CC) $(TSAN_CJSON_CFLAGS) -I$(LIB_DIR) -c $< -o $@

$(BUILD)/test_%.tsan.o: test_%.c test.h | $(BUILD)
	$(CC) $(TSAN_CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/test_queue_tsan: $(BUILD)/test_queue.tsan.o $(BUILD)/queue.tsan.o | $(BUILD)
	$(CC) $(TSAN_CFLAGS) $^ $(TSAN_LDFLAGS) -o $@

$(BUILD)/test_webhook_tsan: $(BUILD)/test_webhook.tsan.o $(BUILD)/webhook.tsan.o $(BUILD)/queue.tsan.o $(BUILD)/cJSON.tsan.o $(BUILD)/logger.tsan.o | $(BUILD)
	$(CC) $(TSAN_CFLAGS) $^ $(TSAN_LDFLAGS) -o $@

$(BUILD)/test_whitelist_tsan: $(BUILD)/test_whitelist.tsan.o $(BUILD)/whitelist.tsan.o $(BUILD)/logger.tsan.o | $(BUILD)
	$(CC) $(TSAN_CFLAGS) $^ $(TSAN_LDFLAGS) -o $@

$(BUILD)/test_commands_tsan: $(BUILD)/test_commands.tsan.o $(BUILD)/commands.tsan.o $(BUILD)/queue.tsan.o $(BUILD)/whitelist.tsan.o $(BUILD)/logger.tsan.o | $(BUILD)
	$(CC) $(TSAN_CFLAGS) $^ $(TSAN_LDFLAGS) -o $@

$(BUILD)/test_logger_tsan: $(BUILD)/test_logger.tsan.o $(BUILD)/logger.tsan.o | $(BUILD)
	$(CC) $(TSAN_CFLAGS) $^ $(TSAN_LDFLAGS) -o $@

TSAN_TESTS := $(BUILD)/test_queue_tsan $(BUILD)/test_webhook_tsan $(BUILD)/test_whitelist_tsan $(BUILD)/test_commands_tsan $(BUILD)/test_logger_tsan

tsan: $(TSAN_TESTS)
	@echo ""
	@echo "======================================"
	@echo "    running TSan tests ..."
	@echo "======================================"
	@fail=0;                                           \
	for t in $(TSAN_TESTS); do                         \
	    echo "";                                       \
	    echo "--- $$t ---";                            \
	    if $$t; then                                   \
	        echo "  PASSED";                           \
	    else                                           \
	        echo "  FAILED";                           \
	        fail=1;                                    \
	    fi;                                            \
	done;                                              \
	echo "";                                           \
	if [ $$fail -eq 0 ]; then                          \
	    echo "======================================"; \
	    echo "  ALL TSan test suites PASSED!";         \
	    echo "======================================"; \
	else                                               \
	    echo "======================================"; \
	    echo "  some TSan tests FAILED!";              \
	    echo "======================================"; \
	    exit 1;                                        \
	fi

clean:
	rm -rf $(BUILD)
