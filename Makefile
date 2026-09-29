.PHONY: all test clean sanitize http-smoke benchmark

CC ?= cc
AR ?= ar
BUILD_DIR ?= build
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror -pthread
LDLIBS += -pthread -lm
MHD_CFLAGS := $(shell pkg-config --cflags libmicrohttpd 2>/dev/null)
MHD_LIBS := $(shell pkg-config --libs libmicrohttpd 2>/dev/null)

# Select the SDK from the same developer directory as the compiler/linker.
ifeq ($(shell uname -s),Darwin)
WADB_SDK ?= $(shell p="$$(xcode-select -p)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"; if test -d "$$p"; then printf '%s' "$$p"; else xcrun --show-sdk-path; fi)
CPPFLAGS += -D_DARWIN_C_SOURCE -isysroot $(WADB_SDK)
LDFLAGS += -isysroot $(WADB_SDK)
endif

CORE_SRCS := $(wildcard src/core/*.c src/storage/*.c src/platform/*.c src/ingest/*.c src/query/*.c)
LIB_SRCS := $(CORE_SRCS)
LIB_OBJS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(LIB_SRCS))
LIB := $(BUILD_DIR)/libwebanalyticsdb.a
SERVER_SRCS := $(wildcard src/server/*.c) vendor/yyjson/yyjson.c
SERVER_OBJS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(SERVER_SRCS))
SERVER_LIB := $(BUILD_DIR)/libwadbserver.a
APP := $(BUILD_DIR)/webanalyticsdb
APP_OBJ := $(BUILD_DIR)/obj/src/main.o
UNITY_OBJ := $(BUILD_DIR)/obj/test/unity/unity.o
TEST_SRCS := $(wildcard tests/test_*.c)
TEST_EXES := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SRCS))
CRASH_OBJS := $(patsubst %.c,$(BUILD_DIR)/crash/%.o,$(CORE_SRCS))
CRASH_LIB := $(BUILD_DIR)/crash/libwebanalyticsdb.a
DEPS := $(LIB_OBJS:.o=.d) $(CRASH_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(APP_OBJ:.o=.d) $(UNITY_OBJ:.o=.d) $(TEST_EXES:=.d)
DEPS += $(BUILD_DIR)/bench-core.d
.SECONDARY: $(UNITY_OBJ)

all: $(APP) $(SERVER_LIB) $(TEST_EXES)

benchmark: $(BUILD_DIR)/bench-core $(APP)

$(BUILD_DIR)/bench-core: tools/bench-core.c $(LIB)
	$(CC) $(CPPFLAGS) -D_DEFAULT_SOURCE $(CFLAGS) $(LDFLAGS) -MMD -MP -MF $@.d $< $(LIB) $(LDLIBS) -o $@

$(BUILD_DIR)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJS) Makefile
	$(RM) $@
	$(AR) rcs $@ $(LIB_OBJS)

# Only this separate archive contains the process-termination/failure hooks.
$(BUILD_DIR)/crash/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWADB_TEST_FAULTS -MMD -MP -c $< -o $@

$(CRASH_LIB): $(CRASH_OBJS) Makefile
	$(RM) $@
	$(AR) rcs $@ $(CRASH_OBJS)

$(BUILD_DIR)/test_crash: tests/test_crash.c $(UNITY_OBJ) $(CRASH_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -MMD -MP -MF $@.d $< $(UNITY_OBJ) $(CRASH_LIB) $(LDLIBS) -o $@

$(APP): $(APP_OBJ) $(SERVER_LIB) $(LIB)
	$(CC) $(LDFLAGS) $(CFLAGS) $^ $(LDLIBS) $(MHD_LIBS) -o $@

$(BUILD_DIR)/web_assets.h: web/index.html web/app.js web/style.css tools/embed-web.py
	python3 tools/embed-web.py $@

$(BUILD_DIR)/obj/src/server/server.o: src/server/server.c $(BUILD_DIR)/web_assets.h
	@pkg-config --exact-version=1.0.10 libmicrohttpd || { echo 'GNU libmicrohttpd 1.0.10 and pkg-config are required; see docs/dependencies.md'; exit 1; }
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(MHD_CFLAGS) -I$(BUILD_DIR) $(CFLAGS) -MMD -MP -c $< -o $@

$(SERVER_LIB): $(SERVER_OBJS) Makefile
	$(RM) $@
	$(AR) rcs $@ $(SERVER_OBJS)

$(BUILD_DIR)/test_api $(BUILD_DIR)/test_jobs $(BUILD_DIR)/test_auth $(BUILD_DIR)/test_server: $(BUILD_DIR)/test_%: tests/test_%.c $(UNITY_OBJ) $(SERVER_LIB) $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -MMD -MP -MF $@.d $< $(UNITY_OBJ) $(SERVER_LIB) $(LIB) $(LDLIBS) $(MHD_LIBS) -o $@

$(BUILD_DIR)/test_%: tests/test_%.c $(UNITY_OBJ) $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -MMD -MP -MF $@.d $< $(UNITY_OBJ) $(LIB) $(LDLIBS) -o $@

# Preserve every executable's status rather than masking failures behind tee.
test: $(TEST_EXES)
	@set -e; for test in $(TEST_EXES); do "$$test"; done

sanitize:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/sanitize CFLAGS='-O1 -g -std=c17 -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' test

http-smoke: $(APP)
	python3 tools/http-stress-smoke.py --executable $(APP) --output $(BUILD_DIR)/http-stress-smoke.json

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
