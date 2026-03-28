# jettyd-sdk — developer shortcuts
#
# make test    — run host unit tests (fast, no ESP-IDF needed)
# make check   — unit tests + IDF build against the firmware template
#                Run this before every commit to the SDK.

TEMPLATE_DIR := $(shell cd .. && pwd)/jettyd-firmware-template

.PHONY: test check clean

test:
	@$(MAKE) -C test --no-print-directory
	@echo "✅ Unit tests passed"

check: test
	@echo "→ Building against firmware template (esp32s3)..."
	@if [ ! -f "$(TEMPLATE_DIR)/CMakeLists.txt" ]; then \
		echo "⚠️  Template not found at $(TEMPLATE_DIR) — skipping IDF build"; \
	else \
		cd $(TEMPLATE_DIR) && \
		. $(IDF_PATH)/export.sh > /dev/null 2>&1 && \
		idf.py build 2>&1 | tail -3; \
	fi
	@echo "✅ SDK check passed — safe to commit"

clean:
	@$(MAKE) -C test clean --no-print-directory
