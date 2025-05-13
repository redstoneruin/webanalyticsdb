.PHONY: all test clean

CC = gcc
CFLAGS = -Wall -Wextra
AR = ar
ARFLAGS = rcs

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
LIB = $(BUILD_DIR)/libwebanalyticsdb.a
APP = $(BUILD_DIR)/webanalyticsdb

SRCS = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
MAIN_SRC = src/main.c
LIB_SRCS = $(filter-out $(MAIN_SRC), $(SRCS))
LIB_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
MAIN_OBJ = $(OBJ_DIR)/main.o

TEST_SRCS = $(wildcard tests/*.c)
TEST_EXES = $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SRCS))

all: $(APP) $(TEST_EXES)

test: $(TEST_EXES)
	for test in $^; do ./$$test; done

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR):
	mkdir -p $@

$(OBJ_DIR):
	mkdir -p $@

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(APP): $(MAIN_OBJ) $(LIB)
	$(CC) -o $@ $^

$(BUILD_DIR)/%: tests/%.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LIB)
