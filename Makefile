CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -MMD -MP -Iinclude
BUILD := build

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))

.PHONY: all test bench clean

all: $(BUILD)/libogfn_bots.a

$(BUILD)/libogfn_bots.a: $(OBJS)
	ar rcs $@ $^

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/bots_test
	./$(BUILD)/bots_test

bench: $(BUILD)/bench
	./$(BUILD)/bench

$(BUILD)/bench: bench/bench.cpp $(BUILD)/libogfn_bots.a
	$(CXX) $(CXXFLAGS) $< -L$(BUILD) -logfn_bots -o $@

$(BUILD)/bots_test: tests/bots_test.cpp $(BUILD)/libogfn_bots.a
	$(CXX) $(CXXFLAGS) $< -L$(BUILD) -logfn_bots -o $@

clean:
	rm -rf $(BUILD)

-include $(wildcard *.d)
