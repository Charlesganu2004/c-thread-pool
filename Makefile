ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -O2
LDLIBS ?=

override CPPFLAGS += -Iinclude
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

ifeq ($(OS),Windows_NT)
TARGET := thread_pool_demo.exe
override CPPFLAGS += -D_WIN32_WINNT=0x0600
else
TARGET := thread_pool_demo
override LDLIBS += -pthread
endif

SRC := src/main.c src/thread_pool.c
OBJ := $(SRC:.c=.o)

# The regression test uses pthreads directly, so it builds on POSIX systems only.
TEST_DEPS := tests/destroy_race.c src/thread_pool.c include/thread_pool.h
TEST_BINS := tests/destroy_race tests/destroy_race_asan tests/destroy_race_tsan

# Command prefix for the ThreadSanitizer run, for example
# TSAN_RUN="setarch $(uname -m) -R" on WSL.
TSAN_RUN ?=

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

src/%.o: src/%.c include/thread_pool.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

test: $(TEST_DEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/destroy_race.c src/thread_pool.c -o tests/destroy_race $(LDLIBS)
	./tests/destroy_race

test-asan: $(TEST_DEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined tests/destroy_race.c src/thread_pool.c -o tests/destroy_race_asan $(LDLIBS)
	./tests/destroy_race_asan

test-tsan: $(TEST_DEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=thread tests/destroy_race.c src/thread_pool.c -o tests/destroy_race_tsan $(LDLIBS)
	$(TSAN_RUN) ./tests/destroy_race_tsan

clean:
	$(RM) $(OBJ) $(TARGET) $(TEST_BINS)

.PHONY: all clean run test test-asan test-tsan
