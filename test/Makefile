# Jettyd SDK — Host Unit Tests
# Compiles and runs tests natively (no ESP-IDF, no hardware).
#
# Usage:
#   make          — build and run all tests
#   make test     — same
#   make clean    — remove build artifacts

CC      := cc
CFLAGS  := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g \
           -I. \
           -Imocks \
           -I../jettyd/include \
           -I../drivers/button/include \
           -I../drivers/led/include \
           -I../drivers/camera/include \
           -I../drivers/servo/include

SDK_SRC := ../jettyd/src/driver_registry.c

.PHONY: all test clean

all: test

test: build/test_drivers build/test_button build/test_manifest build/test_mcp_tools build/test_camera build/test_servo build/test_wifi_reconfigure
	@echo ""
	@echo "Running test_drivers..."
	@./build/test_drivers
	@echo ""
	@echo "Running test_button..."
	@./build/test_button
	@echo ""
	@echo "Running test_manifest..."
	@./build/test_manifest
	@echo ""
	@echo "Running test_mcp_tools..."
	@./build/test_mcp_tools
	@echo ""
	@echo "Running test_camera..."
	@./build/test_camera
	@echo ""
	@echo "Running test_servo..."
	@./build/test_servo
	@echo ""
	@echo "Running test_wifi_reconfigure..."
	@./build/test_wifi_reconfigure
	@echo ""
	@echo "✅ All test suites passed"

build/test_drivers: test_drivers.c $(SDK_SRC) | build
	$(CC) $(CFLAGS) -o $@ $^

build/test_button: test_button.c | build
	$(CC) $(CFLAGS) -o $@ $<

build/test_servo: test_servo.c | build
	$(CC) $(CFLAGS) -o $@ $<

build/test_wifi_reconfigure: test_wifi_reconfigure.c ../jettyd/src/wifi.c | build
	$(CC) $(CFLAGS) -o $@ $<

build/test_manifest: test_manifest.c $(SDK_SRC) | build
	$(CC) $(CFLAGS) -o $@ $^

build/test_mcp_tools: test_mcp_tools.c $(SDK_SRC) | build
	$(CC) $(CFLAGS) -o $@ $^

build/test_camera: test_camera.c $(SDK_SRC) | build
	$(CC) $(CFLAGS) -DCONFIG_JETTYD_CAMERA_SUPPORTED=1 -o $@ $^

build:
	mkdir -p build

clean:
	rm -rf build/
