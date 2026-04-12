# Jettyd SDK — Host Unit Tests
# Compiles and runs tests natively (no ESP-IDF, no hardware).
#
# Usage:
#   make          — build and run all tests
#   make test     — same
#   make clean    — remove build artifacts

CC      := cc
CFLAGS  := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g \
           -I. \
           -Imocks \
           -I../jettyd/include \
           -I../drivers/button/include \
           -I../drivers/led/include

SDK_SRC := ../jettyd/src/driver_registry.c

.PHONY: all test clean

all: test

test: build/test_drivers build/test_button
	@echo ""
	@echo "Running test_drivers..."
	@./build/test_drivers
	@echo ""
	@echo "Running test_button..."
	@./build/test_button
	@echo ""
	@echo "✅ All test suites passed"

build/test_drivers: test_drivers.c $(SDK_SRC) | build
	$(CC) $(CFLAGS) -o $@ $^

build/test_button: test_button.c | build
	$(CC) $(CFLAGS) -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build/
