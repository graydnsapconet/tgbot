CC      := gcc
CFLAGS  := -Wall -Wextra -pedantic -std=c11 -O2           \
           -Werror                                         \
           -Wformat=2 -Wformat-truncation -Wformat-overflow\
           -Wshadow -Wconversion -Wdouble-promotion        \
           -Wnull-dereference -Wstrict-prototypes          \
           -Wmissing-prototypes -Wold-style-definition     \
           -D_FORTIFY_SOURCE=2 -DNDEBUG                    \
           -fstack-protector-strong                        \
           -fPIE                                           \
           -flto                                           \
           -march=native                                   \
           -fdata-sections -ffunction-sections             \
           -fvisibility=hidden                             \
           -pipe

LDFLAGS := -lcurl -lpthread -lmicrohttpd -pie -Wl,-z,relro,-z,now \
           -Wl,--gc-sections                                       \
           -flto -s

# Vendored cJSON triggers many warnings we cannot fix upstream.
# Build it with relaxed flags so -Werror applies only to our code.
CJSON_CFLAGS := -Wall -std=c11 -O2 -D_FORTIFY_SOURCE=2    \
                -fstack-protector-strong -fPIE             \
                -flto -march=native -pipe

SRC_DIR := src
LIB_DIR := lib
BUILD   := build

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS)))
OBJS += $(BUILD)/cJSON.o
OBJS += $(BUILD)/ini.o
BIN  := $(BUILD)/tgbot

# ── Debug / sanitiser build ───────────────────────────────────────────
DEBUG_CFLAGS := $(filter-out -O2,$(CFLAGS)) -O0 -g3        \
                -fno-omit-frame-pointer                     \
                -fsanitize=address,undefined
DEBUG_LDFLAGS := $(LDFLAGS) -fsanitize=address,undefined

# ── Targets ───────────────────────────────────────────────────────────
.PHONY: all clean run debug analyze cppcheck tidy format check install uninstall

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $< -o $@

$(BUILD)/cJSON.o: $(LIB_DIR)/cJSON.c | $(BUILD)
	$(CC) $(CJSON_CFLAGS) -I$(LIB_DIR) -c $< -o $@

$(BUILD)/ini.o: $(LIB_DIR)/ini.c | $(BUILD)
	$(CC) $(CJSON_CFLAGS) -I$(LIB_DIR) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD)

run: $(BIN)
	./$(BIN)

# ── Debug build (ASan + UBSan) ────────────────────────────────────────
debug: clean
	mkdir -p $(BUILD)
	@for src in $(SRCS); do \
	    $(CC) $(DEBUG_CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR) -c $$src \
	        -o $(BUILD)/$$(basename $${src%.c}.o); \
	done
	$(CC) $(CJSON_CFLAGS) -O0 -g3 -fno-omit-frame-pointer \
	    -fsanitize=address,undefined \
	    -I$(LIB_DIR) -c $(LIB_DIR)/cJSON.c -o $(BUILD)/cJSON.o
	$(CC) $(CJSON_CFLAGS) -O0 -g3 -fno-omit-frame-pointer \
	    -fsanitize=address,undefined \
	    -I$(LIB_DIR) -c $(LIB_DIR)/ini.c -o $(BUILD)/ini.o
	$(CC) $(DEBUG_CFLAGS) $(BUILD)/*.o $(DEBUG_LDFLAGS) -o $(BIN)
	@echo "=== debug build ready (ASan + UBSan enabled) ==="

# ── GCC static analyser ──────────────────────────────────────────────
analyze:
	@echo "=== gcc -fanalyzer ==="
	@for src in $(SRCS); do \
	    echo "  $$src"; \
	    $(CC) $(CFLAGS) -fanalyzer -I$(LIB_DIR) -I$(SRC_DIR) \
	        -fsyntax-only $$src; \
	done
	@echo "=== analysis complete ==="

# ── cppcheck ─────────────────────────────────────────────────────────
cppcheck:
	@echo "=== cppcheck ==="
	cppcheck --enable=all --std=c11 --language=c \
	    --suppress=missingIncludeSystem          \
	    --suppress=unusedFunction                \
	    --error-exitcode=1                       \
	    -I$(LIB_DIR) -I$(SRC_DIR) $(SRC_DIR)/
	@echo "=== cppcheck clean ==="

# ── clang-tidy ───────────────────────────────────────────────────────
tidy:
	@echo "=== clang-tidy ==="
	@for src in $(SRCS); do \
	    echo "  $$src"; \
	    clang-tidy $$src -- $(CFLAGS) -I$(LIB_DIR) -I$(SRC_DIR); \
	done
	@echo "=== tidy complete ==="

# ── clang-format (dry-run check) ─────────────────────────────────────
format:
	@echo "=== clang-format check ==="
	clang-format --dry-run --Werror $(SRC_DIR)/*.c $(SRC_DIR)/*.h
	@echo "=== formatting clean ==="

# ── Meta: run all quality gates ──────────────────────────────────────
check: format tidy cppcheck analyze all
	@echo ""
	@echo "======================================="
	@echo "  All quality gates passed."
	@echo "======================================="

PREFIX   ?= /usr/local
CONFDIR  ?= /etc/tgbot
LOGDIR   ?= /var/log/tgbot
SYSTEMD  ?= /etc/systemd/system
SVC_NAME := tgbot-service.service

# ── Install ──────────────────────────────────────────────────────────
install: $(BIN)
	@echo "=== installing tgbot ==="
	# create tgbot user/group if missing
	@id -u tgbot >/dev/null 2>&1 || \
	    useradd --system --no-create-home --shell /usr/sbin/nologin tgbot
	install -d -m 0755 $(CONFDIR)
	install -d -m 0755 -o tgbot -g tgbot $(LOGDIR)
	install -m 0755 $(BIN) $(PREFIX)/bin/tgbot
	@test -f $(CONFDIR)/tgbot.ini || \
	    install -m 0640 -o tgbot -g tgbot tgbot.ini.example $(CONFDIR)/tgbot.ini
	@test -f $(CONFDIR)/whitelist.txt || \
	    install -m 0640 -o tgbot -g tgbot /dev/null $(CONFDIR)/whitelist.txt
	install -m 0644 $(SVC_NAME) $(SYSTEMD)/$(SVC_NAME)
	systemctl daemon-reload
	@echo "=== install complete ==="
	@echo "  enable:  systemctl enable $(SVC_NAME)"
	@echo "  start:   tgbot start"

# ── Uninstall ────────────────────────────────────────────────────────
uninstall:
	@echo "=== uninstalling tgbot ==="
	-systemctl stop $(SVC_NAME) 2>/dev/null
	-systemctl disable $(SVC_NAME) 2>/dev/null
	rm -f $(SYSTEMD)/$(SVC_NAME)
	rm -f $(PREFIX)/bin/tgbot
	systemctl daemon-reload
	@echo "=== uninstall complete ==="
	@echo "  config remains at $(CONFDIR) — remove manually if desired"
