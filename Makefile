CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2 -g -fPIC -fno-omit-frame-pointer -D_GNU_SOURCE
CXXFLAGS = -Wall -Wextra -O2 -g -fPIC -fno-omit-frame-pointer -D_GNU_SOURCE -std=c++17
LDFLAGS = -shared -ldl -lpthread

SRC_DIR = src
BIN_DIR = bin
TEST_DIR = tests

SRCS_C = $(SRC_DIR)/preload.c $(SRC_DIR)/bootstrap.c $(SRC_DIR)/tracker.c $(SRC_DIR)/stacktrace.c $(SRC_DIR)/report.c $(SRC_DIR)/analyzer.c
SRCS_CXX = $(SRC_DIR)/preload_cpp.cpp
OBJS = $(SRCS_C:.c=.o) $(SRCS_CXX:.cpp=.o)
LIB = $(BIN_DIR)/libvortex.so

TEST_SRC = $(TEST_DIR)/test_leaks.c
TEST_BIN = $(BIN_DIR)/test_leaks

GOD_MODE_SRC = $(TEST_DIR)/test_mode.c
GOD_MODE_BIN = $(BIN_DIR)/test_mode

SERVER_SRC = $(SRC_DIR)/server.c
SERVER_BIN = $(BIN_DIR)/vortex_server

MULTITHREAD_SRC = $(TEST_DIR)/test_multithread.c
MULTITHREAD_BIN = $(BIN_DIR)/test_multithread

BENCHMARK_SRC = $(TEST_DIR)/benchmark.c
BENCHMARK_BIN = $(BIN_DIR)/benchmark

TEST_CPP_SRC = $(TEST_DIR)/test_cpp.cpp
TEST_CPP_BIN = $(BIN_DIR)/test_cpp

ROBUSTNESS_SRC = $(TEST_DIR)/test_robustness.c
ROBUSTNESS_BIN = $(BIN_DIR)/test_robustness

all: $(LIB) $(TEST_BIN) $(GOD_MODE_BIN) $(SERVER_BIN) $(MULTITHREAD_BIN) $(BENCHMARK_BIN) $(TEST_CPP_BIN) $(ROBUSTNESS_BIN)

$(LIB): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

$(TEST_BIN): $(TEST_SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -rdynamic $< -o $@

$(GOD_MODE_BIN): $(GOD_MODE_SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -rdynamic $< -o $@

$(SERVER_BIN): $(SERVER_SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ -lpthread

$(MULTITHREAD_BIN): $(MULTITHREAD_SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -rdynamic $< -o $@ -lpthread

$(BENCHMARK_BIN): $(BENCHMARK_SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -rdynamic $< -o $@ -lpthread

$(TEST_CPP_BIN): $(TEST_CPP_SRC)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -rdynamic $< -o $@

$(ROBUSTNESS_BIN): $(ROBUSTNESS_SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -rdynamic $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

RUNNER_SRC = $(TEST_DIR)/run_tests.c
RUNNER_BIN = $(BIN_DIR)/run_tests

test: all
	@echo "Compiling and running C Test Suite Runner..."
	$(CC) $(CFLAGS) $(RUNNER_SRC) -o $(RUNNER_BIN)
	./$(RUNNER_BIN)

clean:
	rm -f $(OBJS) $(LIB) $(TEST_BIN) $(GOD_MODE_BIN) $(SERVER_BIN) $(MULTITHREAD_BIN) $(BENCHMARK_BIN) $(TEST_CPP_BIN) $(ROBUSTNESS_BIN) $(RUNNER_BIN) vortex_report*.json test_report*.json

.PHONY: all test clean