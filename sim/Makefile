CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
INC      := -Isrc
SRC      := src/util.cpp src/workload.cpp src/memory.cpp src/hbm.cpp src/bulk.cpp src/ab.cpp src/system.cpp
OBJ      := $(SRC:src/%.cpp=build/%.o)

all: bin/dx100sim bin/dx100_tests

build/%.o: src/%.cpp src/sim.hpp src/hbm.hpp src/hbm.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

bin/dx100sim: $(OBJ) build/main.o
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@

build/tests.o: tests/tests.cpp src/sim.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

bin/dx100_tests: $(OBJ) build/tests.o
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@

test: bin/dx100_tests
	./bin/dx100_tests

clean:
	rm -rf build bin

.PHONY: all test clean
