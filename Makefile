# Builds the plugin: x86_64 in WSL for tests, aarch64 natively on the Pi.

CXX := g++
BUILD := build
MOT := third_party/MoT
LV2_DIR := $(HOME)/.lv2
PI := pistomp

# chowdsp_wdf is our own submodule, on the commit MoT pins (tests/check_pins.sh checks that).
INCLUDES := -Isrc -isystem $(MOT)/src -isystem third_party/chowdsp_wdf/include
FLAGS := -std=c++17 -O3 -fPIC -fvisibility=hidden -Wall -Wextra $(INCLUDES)

ifeq ($(shell uname -m),aarch64)
FLAGS += -mcpu=cortex-a76
endif

HEADERS := $(wildcard src/*.h) $(wildcard $(MOT)/src/dsp/*.h)

# The bundle gets its own folder, so LV2_PATH can point at it without lilv tripping over anything else.
BUNDLE_DIR := $(BUILD)/lv2

# The face (metal, fonts) is ours. The art for knobs, switches and LEDs stays in the submodule and is
# copied into the bundle.
MODGUI_FILES := $(wildcard modgui/*.html modgui/*.css modgui/*.png modgui/*.svg modgui/*.ttf modgui/*.txt) \
	$(addprefix $(MOT)/assets/ui/,bakelite_knob.png trim_knob.png vol_trim.png \
		bezel_led_off.png bezel_led_red.png bezel_led_yellow.png footswitch_up.png footswitch_down.png \
		switch_up.png switch_mid.png switch_down.png)

# Pedalboards store plugins by URI, so changing it breaks every saved pedalboard that uses the plugin.
NAME := twin-crowns
URI := urn:ignatzmcjockel:$(NAME)
BUNDLE := $(BUNDLE_DIR)/$(NAME).lv2

all: $(BUNDLE)/$(NAME).so $(BUNDLE)/$(NAME).ttl $(BUNDLE)/manifest.ttl $(BUNDLE)/modgui.ttl \
	$(BUNDLE)/presets.ttl $(BUILD)/$(NAME)-modgui-copied

# Starts from an empty folder, so files dropped from the list don't linger in the bundle.
$(BUILD)/$(NAME)-modgui-copied: $(MODGUI_FILES)
	rm -rf $(BUNDLE)/modgui && mkdir -p $(BUNDLE)/modgui
	cp $^ $(BUNDLE)/modgui/
	touch $@

$(BUNDLE)/$(NAME).so: src/crowns.cpp $(HEADERS)
	mkdir -p $(@D)
	$(CXX) $(FLAGS) -DCROWNS_URI='"$(URI)"' -shared -Wl,--no-undefined $< -o $@ -lm

$(BUNDLE)/%.ttl: ttl/%.ttl.in
	mkdir -p $(@D)
	sed -e 's|@URI@|$(URI)|g' -e 's|@NAME@|$(NAME)|g' $< > $@

.PHONY: all test nulltest install clean deploy

$(BUILD)/tests/test_%: tests/test_%.cpp $(HEADERS)
	mkdir -p $(@D)
	$(CXX) $(FLAGS) $< -o $@

test: $(BUILD)/tests/test_halfband $(BUILD)/tests/test_engine
	sh tests/check_pins.sh
	$(BUILD)/tests/test_halfband
	$(BUILD)/tests/test_engine

# Needs the reference from tests/nulltest/setup_reference.sh.
nulltest: all
	python3 tests/nulltest/nulltest.py

# Removes before copying: writing over a .so that mod-host has loaded would crash mod-host.
# kot.lv2 is the bundle from before the rename to Twin Crowns, with the old URI.
install: all
	mkdir -p $(LV2_DIR)
	rm -rf $(LV2_DIR)/kot.lv2
	rm -rf $(LV2_DIR)/$(NAME).lv2 && cp -r $(BUNDLE) $(LV2_DIR)/

clean:
	rm -rf $(BUILD)

# Copies only what the build needs, then builds on the Pi. Needs the `pistomp` SSH host entry.
deploy:
	rsync -aR --delete Makefile src ttl tests modgui $(MOT)/src/dsp $(MOT)/assets/ui third_party/chowdsp_wdf/include $(PI):twin-crowns/
	ssh -t $(PI) 'cd twin-crowns && make -j4 && make install && sudo systemctl restart mod-host mod-ui'
