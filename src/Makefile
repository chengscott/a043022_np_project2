CXX=g++
CXXFLAGS=-std=c++11 -Wall

all: np_simple np_single_proc

%: %.cc
	$(CXX) $(CXXFLAGS) $< -o $@

.PHONY: clean
clean:
	rm -rf np_simple np_single_proc np_multi_proc

.PHONY: format
format:
	clang-format -i *.cc

.PHONY: check
check:
	clang-tidy -checks='bugprone-*,clang-analyzer-*,modernize-*' *.cc -- -std=c++11
