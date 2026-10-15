# Select the native build while preserving upstream Linux source paths.
ASCOLTO_PLATFORM ?= $(shell uname -s)

ifeq ($(ASCOLTO_PLATFORM),Darwin)
include Makefile.macos
else ifeq ($(ASCOLTO_PLATFORM),Linux)
include Makefile
else
$(error Unsupported platform '$(ASCOLTO_PLATFORM)'; expected Darwin or Linux)
endif

.PHONY: test-portable
test-portable:
	@mkdir -p bin
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude src/capture_buffer.c src/audio_queue.c src/audio_processing.c tests/portable_main.c -o bin/ascolto-portable-test -lpthread -lm
	./bin/ascolto-portable-test

# Compare both algorithms against identical PCM using the host's Needle library.
ifeq ($(ASCOLTO_PLATFORM),Darwin)
COMPARE_NEEDLE := vendor/needle/libneedle.a -lc++
COMPARE_HEADERS := -Ivendor/needle
COMPARE_MODEL := models/whistle.cact
else
COMPARE_NEEDLE := third_party/whistle/libneedle.so -Wl,-rpath,'$$ORIGIN/../third_party/whistle'
COMPARE_HEADERS := -Ithird_party/whistle
COMPARE_MODEL := third_party/whistle/whistle.cact
endif
FIXTURE_DIR ?= tests/fixtures
.PHONY: compare-diarizers test-recap
compare-diarizers: deps
	@mkdir -p bin
	$(CC) -std=c11 -O2 -Wall -Wextra -Iinclude $(COMPARE_HEADERS) -DASCOLTO_COMPARE_MACOS src/macos/diarizer.c src/macos/whistle_engine.c src/wav_audio.c src/audio_processing.c tests/compare_diarizers.c -o bin/compare-diarizer-macos $(COMPARE_NEEDLE) -lpthread -lm
	$(CC) -std=c11 -O2 -Wall -Wextra -Iinclude $(COMPARE_HEADERS) src/diarizer.c src/whistle_engine.c src/types_config.c tests/compare_diarizers.c -o bin/compare-diarizer-linux $(COMPARE_NEEDLE) -lpthread -lm
	./bin/compare-diarizer-macos $(COMPARE_MODEL) $(FIXTURE_DIR)
	./bin/compare-diarizer-linux $(COMPARE_MODEL) $(FIXTURE_DIR)

RECAP_PYTHON ?= $(if $(wildcard .venv/bin/python3),.venv/bin/python3,python3)
.PHONY: recap-deps
recap-deps:
	python3 -m venv .venv
	.venv/bin/python3 -m pip install -r scripts/requirements-recap.txt

test-recap:
	$(RECAP_PYTHON) -m unittest discover -s tests -p test_recap.py
