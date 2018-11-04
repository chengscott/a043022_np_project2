all: np_simple np_single_proc

np_simple:
	g++ np_simple.cc -o np_simple -std=c++11 -Wall

np_single_proc:
	g++ np_single_proc.cc -o np_single_proc -std=c++11 -Wall

np_multi_proc:
	g++ np_multi_proc.cc -o np_multi_proc -std=c++11 -Wall

clean:
	rm -rf np_simple np_single_proc np_multi_proc

format:
	clang-format -i *.cc
