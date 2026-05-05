CXX=g++
CXXFLAGS=-std=c++17 -O2

all:
	$(CXX) main.cpp proxy.cpp -o tcp_proxy

clean:
	rm -f tcp_proxy