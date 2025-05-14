.PHONY: all test clean

CC = gcc
CFLAGS = -Wall -Wextra
AR = ar
ARFLAGS = rcs

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
LIB = $(BUILD_DIR)/libwebanalyticsdb.a
APP = $(BUILD_DIR)/webanalyticsdb
UNITY_SRC = test/unity/unity.c
UNITY_OBJ = $(OBJ_DIR)/unity.o

SRCS = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
MAIN_SRC = src/main.c
LIB_SRCS = $(filter-out $(MAIN_SRC), $(SRCS))
LIB_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
MAIN_OBJ = $(OBJ_DIR)/main.o

TEST_SRCS = $(wildcard tests/*.c)
ifneq ($(LIB_SRCS),)
TEST_EXES = $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SRCS))
else
TEST_EXES =
endif

APP_DEPS = $(if $(LIB_SRCS),$(MAIN_OBJ) $(LIB),)

all: $(APP) $(TEST_EXES)

test: $(TEST_EXES)
	@if [ -z "$(TEST_EXES)" ]; then \
		echo "No tests found or only main.c present"; \
	else \
		echo "Running tests..."; \
		for test in $(TEST_EXES); do \
			./$$test | tee -a $(BUILD_DIR)/test_results.txt; \
		done; \
		echo "\nTest Summary:"; \
		grep -E "FAIL|PASS|IGNORE" $(BUILD_DIR)/test_results.txt || echo "No test results found"; \
		rm -f $(BUILD_DIR)/test_results.txt; \
	fi

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR):
	mkdir -p $@

$(OBJ_DIR):
	mkdir -p $@

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/unity.o: $(UNITY_SRC) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(APP): $(APP_DEPS) | $(BUILD_DIR)
	@if [ -z "$(LIB_SRCS)" ]; then \
		$(CC) $(CFLAGS) $(MAIN_SRC) -o $@; \
	else \
		$(CC) -o $@ $(MAIN_OBJ) $(LIB); \
	fi

$(BUILD_DIR)/%: tests/%.c $(UNITY_OBJ) $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(UNITY_OBJ) $(LIB) -o $@
