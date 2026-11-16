CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -g -Icommon $(EXTRA)
LDFLAGS = $(EXTRA)

BUILD = build
BIN   = bin

COMMON_OBJ  = $(patsubst %.c,$(BUILD)/%.o,$(wildcard common/*.c))
TRACKER_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(wildcard tracker/*.c))
PEER_OBJ    = $(patsubst %.c,$(BUILD)/%.o,$(wildcard peer/*.c))
LIBCOMMON   = $(BUILD)/libcommon.a

PROGS = $(BIN)/tracker $(BIN)/peer $(BIN)/fake_peer $(BIN)/fake_tracker \
        $(BIN)/unit_test

.PHONY: all test asan clean

all: $(PROGS)

$(LIBCOMMON): $(COMMON_OBJ)
	ar rcs $@ $^

$(BIN)/tracker: $(TRACKER_OBJ) $(LIBCOMMON)
	@mkdir -p $(BIN)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BIN)/peer: $(PEER_OBJ) $(LIBCOMMON)
	@mkdir -p $(BIN)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BIN)/%: $(BUILD)/tests/%.o $(LIBCOMMON)
	@mkdir -p $(BIN)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Giữ file .o của tests (make coi là file trung gian và xoá nếu không khai báo)
.SECONDARY:

test: all
	$(BIN)/unit_test
	tests/e2e_join.sh
	tests/e2e_catalog.sh
	tests/e2e_transfer.sh

# Build lại với AddressSanitizer + UBSan: make asan && make test
asan:
	$(MAKE) clean
	$(MAKE) EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer"

clean:
	rm -rf $(BUILD) $(BIN)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
