CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
BUILD    := build
LIB_SRC  := src/config.cpp src/graph.cpp src/supervisor.cpp
LIB_OBJ  := $(LIB_SRC:src/%.cpp=$(BUILD)/%.o)

all: $(BUILD)/processpilot $(BUILD)/ppctl

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/processpilot: $(LIB_OBJ) $(BUILD)/main.o
	$(CXX) $^ -o $@

$(BUILD)/ppctl: $(BUILD)/ppctl.o
	$(CXX) $^ -o $@

$(BUILD)/unit_tests: tests/unit_tests.cpp $(BUILD)/config.o $(BUILD)/graph.o
	$(CXX) $(CXXFLAGS) -Isrc $^ -o $@

test: all $(BUILD)/unit_tests
	./$(BUILD)/unit_tests
	bash tests/integration.sh

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
