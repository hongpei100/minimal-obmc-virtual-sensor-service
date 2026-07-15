CXX ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra
PKGS := sdbusplus

sensord: sensord.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(shell pkg-config --cflags --libs $(PKGS))

clean:
	rm -f sensord