# Rack SDK location; unpack Rack-SDK-<version>-<os>-<cpu>.zip into sdk/ or point RACK_DIR elsewhere.
RACK_DIR ?= sdk/Rack-SDK

FLAGS += -Idsp
SOURCES += $(wildcard src/*.cpp) dsp/hchip.cpp dsp/emu_filters.cpp
DISTRIBUTABLES += res

include $(RACK_DIR)/plugin.mk

# The DSP tests (no Rack needed)
test:
	$(MAKE) -C test check

.PHONY: test
