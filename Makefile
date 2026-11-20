TS3_SDK_DIR ?= /workspace/.ts3-dev/sdk
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
BUILD := build
VERSION := $(shell awk '$$2 == "VERSION" { print $$3; exit }' CMakeLists.txt)
CORE := src/controller.cpp src/udp_gate.cpp
HEADERS := $(wildcard src/*.h)
.PHONY: all test clean
all: $(BUILD)/ghostuser.so
$(BUILD):
	mkdir -p $@
$(BUILD)/ghostuser.so: src/plugin.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC -shared -fvisibility=hidden -Isrc -I$(TS3_SDK_DIR)/include src/plugin.cpp $(CORE) -o $@
$(BUILD)/controller_test: tests/controller_test.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc tests/controller_test.cpp $(CORE) -o $@
$(BUILD)/udp_gate_test: tests/udp_gate_test.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc tests/udp_gate_test.cpp $(CORE) -pthread -o $@
$(BUILD)/plugin_menu_smoke: tests/plugin_menu_smoke.cpp src/plugin_api.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -I$(TS3_SDK_DIR)/include $< -ldl -o $@
$(BUILD)/plugin_connection_test: tests/plugin_connection_test.cpp src/plugin.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -I$(TS3_SDK_DIR)/include $< $(CORE) -o $@
test: $(BUILD)/controller_test $(BUILD)/udp_gate_test $(BUILD)/ghostuser.so $(BUILD)/plugin_menu_smoke $(BUILD)/plugin_connection_test
	./$(BUILD)/controller_test
	./$(BUILD)/udp_gate_test
	./$(BUILD)/plugin_menu_smoke $(BUILD)/ghostuser.so $(VERSION)
	./$(BUILD)/plugin_connection_test
clean:
	rm -rf $(BUILD)
