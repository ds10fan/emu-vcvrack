# Offline tests of the DSP core (no Rack needed).
#   make            builds and runs hchip_test against expected/vectors.txt
CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra
DSP := ../dsp/hchip.cpp ../dsp/emu_filters.cpp
HDR := ../dsp/hchip.hpp ../dsp/emu_filters.hpp

all: check ds10 gchip minty

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

build/gchip_test: gchip_test.cpp ../dsp/gchip.cpp ../dsp/gchip.hpp ../dsp/gchip_proto.inc ../dsp/wav.cpp ../dsp/wav.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ gchip_test.cpp ../dsp/gchip.cpp ../dsp/wav.cpp

gchip: build/gchip_test
	./build/gchip_test

build/mintyseq_test: mintyseq_test.cpp ../dsp/mintyseq.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ mintyseq_test.cpp

minty: build/mintyseq_test
	./build/mintyseq_test

clean:
	rm -rf build

.PHONY: all check ds10 gchip minty clean

# Headless test of the Rack module against the SDK's libRack.so (needs ../sdk/Rack-SDK)
RACK_DIR ?= ../sdk/Rack-SDK
smoke: build/smoke
	./build/smoke

build/smoke: smoke/smoke.cpp ../src/HChipFilter.cpp ../src/GChip.cpp ../src/MintySeq.cpp ../dsp/mintyseq.hpp $(DSP) $(HDR) ../dsp/gchip.cpp ../dsp/wav.cpp
	@mkdir -p build
	$(CXX) -std=c++11 -O2 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -I../dsp -o $@ smoke/smoke.cpp $(DSP) ../dsp/gchip.cpp ../dsp/wav.cpp -L$(RACK_DIR) -lRack -Wl,-rpath,$(abspath $(RACK_DIR))

.PHONY: smoke
