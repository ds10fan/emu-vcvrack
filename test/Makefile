# Offline tests of the DSP core (no Rack needed).
#   make            builds and runs hchip_test against expected/vectors.txt
CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra
DSP := ../dsp/hchip.cpp ../dsp/emu_filters.cpp
HDR := ../dsp/hchip.hpp ../dsp/emu_filters.hpp

all: check ds10

build/hchip_test: hchip_test.cpp $(DSP) $(HDR)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ hchip_test.cpp $(DSP)

check: build/hchip_test
	./build/hchip_test expected/vectors.txt

build/ds10_test: ds10_test.cpp ../dsp/ds10_drive.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ ds10_test.cpp

ds10: build/ds10_test
	./build/ds10_test

clean:
	rm -rf build

.PHONY: all check ds10 clean

# Headless test of the Rack module against the SDK's libRack.so (needs ../sdk/Rack-SDK)
RACK_DIR ?= ../sdk/Rack-SDK
smoke: build/smoke
	./build/smoke

build/smoke: smoke/smoke.cpp ../src/HChipFilter.cpp $(DSP) $(HDR)
	@mkdir -p build
	$(CXX) -std=c++11 -O2 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -I../dsp -o $@ smoke/smoke.cpp $(DSP) -L$(RACK_DIR) -lRack -Wl,-rpath,$(abspath $(RACK_DIR))

.PHONY: smoke
