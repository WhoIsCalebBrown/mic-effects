CXX ?= g++
CXXFLAGS ?= -O2 -g0 -std=c++17 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS += -Isrc -Ithird_party $(shell pkg-config --cflags libpipewire-0.3)
LDFLAGS += $(shell pkg-config --libs libpipewire-0.3) -lpthread

BUILD ?= build
SRC := src/main.cpp src/audio.cpp src/mic.cpp src/server.cpp
OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRC))
BIN := $(BUILD)/mic-effects-server

all: $(BIN)

test: $(BUILD)/tst_pitch_bypass $(BUILD)/tst_audio_ring
	$(BUILD)/tst_pitch_bypass
	$(BUILD)/tst_audio_ring

$(BUILD)/tst_audio_ring: ../tests/tst_audio_ring.cpp src/audio_ring.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ ../tests/tst_audio_ring.cpp

$(BUILD)/tst_pitch_bypass: ../tests/tst_pitch_bypass.cpp src/mic.cpp src/mic.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ ../tests/tst_pitch_bypass.cpp src/mic.cpp

$(BIN): $(OBJ)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.cpp src/*.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
