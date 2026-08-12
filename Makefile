CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2 -g -fPIC -D_GNU_SOURCE
CXXFLAGS = -Wall -Wextra -O2 -g -fPIC -D_GNU_SOURCE
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

all: $(LIB) $(TEST_BIN) $(GOD_MODE_BIN) $(SERVER_BIN)

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

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: all
	@echo "Running tests with LD_PRELOAD..."
	LD_PRELOAD=./$(LIB) ./$(TEST_BIN)

clean:
	rm -f $(OBJS) $(LIB) $(TEST_BIN) $(GOD_MODE_BIN) $(SERVER_BIN) vortex_report*.json

.PHONY: all test clean