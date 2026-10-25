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
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude src/capture_buffer.c src/audio_queue.c src/audio_processing.c tests/capture_buffer_verify.c tests/portable_main.c -o bin/ascolto-portable-test -lpthread -lm
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
	$(CC) -std=c11 -O2 -Wall -Wextra -Iinclude $(COMPARE_HEADERS) src/diarizer.c src/whistle_engine.c src/types_config.c src/filesystem.c src/database.c src/storage.c src/ai_settings.c src/audio_processing.c tests/compare_diarizers.c -o bin/compare-diarizer-linux $(COMPARE_NEEDLE) -lpthread -lm -lsqlite3
	./bin/compare-diarizer-macos $(COMPARE_MODEL) $(FIXTURE_DIR)
	./bin/compare-diarizer-linux $(COMPARE_MODEL) $(FIXTURE_DIR)

RECAP_PYTHON ?= $(if $(wildcard .venv/bin/python3),.venv/bin/python3,python3)
.PHONY: recap-deps
recap-deps:
	python3 -m venv .venv
	.venv/bin/python3 -m pip install -r scripts/requirements-recap.txt
	@touch .venv/.ascolto-runtime-ready

test-recap:
	$(RECAP_PYTHON) -m unittest discover -s tests -p test_recap.py
	$(RECAP_PYTHON) -m unittest discover -s tests -p test_ai_workflows.py

.PHONY: test-ai
test-ai:
	$(RECAP_PYTHON) -m unittest discover -s tests -p test_ai_providers.py

.PHONY: test-database test-integration
test-database:
	python3 scripts/generate_database_schema.py --check
	@mkdir -p bin
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude src/database.c tests/database_main.c -lsqlite3 -lpthread -o bin/ascolto-database-test
	./bin/ascolto-database-test
	$(RECAP_PYTHON) -m unittest discover -s tests -p test_database.py

test-integration: test-database
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude src/database.c src/storage.c src/ai_settings.c src/filesystem.c tests/storage_main.c -lsqlite3 -lpthread -o bin/ascolto-storage-test
	./bin/ascolto-storage-test
	$(RECAP_PYTHON) -m unittest discover -s tests -p test_integration.py
	$(RECAP_PYTHON) -m unittest discover -s tests -p test_archive_layout.py

.PHONY: test-diagnostics
test-diagnostics:
	$(RECAP_PYTHON) -m unittest discover -s tests -p test_diagnostics.py

.PHONY: local-stt-deps
local-stt-deps:
	sh scripts/build_local_stt.sh
	@touch build/local-stt/.ready

WITH_LOCAL_STT ?= 1
ifeq ($(WITH_LOCAL_STT),1)
deps: build/local-stt/.ready
build/local-stt/.ready: src/local_stt_manifest.inc scripts/generate_local_stt_manifest.py scripts/build_local_stt.sh src/local_stt/CMakeLists.txt src/local_stt/whisper.c src/local_stt/parakeet.cpp src/local_stt/qwen.c resources/local-stt-manifest.json
	sh scripts/build_local_stt.sh
	@touch $@
ifeq ($(ASCOLTO_PLATFORM),Darwin)
$(BUNDLE_STAMP): build/local-stt/.ready
endif
endif

ifeq ($(ASCOLTO_PLATFORM),Darwin)
STT_TEST_LINK := -lcurl -lsqlite3 -lpthread
STT_TEST_SHARED := -dynamiclib
STT_TEST_SUFFIX := dylib
else
STT_TEST_LINK := -lcurl -lcrypto -ldl -lsqlite3 -lpthread
STT_TEST_SHARED := -shared -fPIC
STT_TEST_SUFFIX := so
endif
.PHONY: test-local-stt
test-local-stt:
	python3 scripts/generate_local_stt_manifest.py --check
	@mkdir -p build/local-stt-tests/local-stt
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude $(STT_TEST_SHARED) tests/local_stt_adapter.c -o build/local-stt-tests/local-stt/whisper-turbo-q5.$(STT_TEST_SUFFIX)
	@cp build/local-stt-tests/local-stt/whisper-turbo-q5.$(STT_TEST_SUFFIX) build/local-stt-tests/local-stt/parakeet-v3-q8.$(STT_TEST_SUFFIX)
	@cp build/local-stt-tests/local-stt/whisper-turbo-q5.$(STT_TEST_SUFFIX) build/local-stt-tests/local-stt/qwen3-asr-1.7b.$(STT_TEST_SUFFIX)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 -DASCOLTO_LOCAL_STT_TEST_MANIFEST -Iinclude src/local_stt.c src/database.c src/storage.c src/filesystem.c src/ai_settings.c tests/local_stt_main.c $(STT_TEST_LINK) -o build/local-stt-tests/test
	./build/local-stt-tests/test

# Explicit network check downloads only one pinned 6 KB public configuration file.
.PHONY: test-local-stt-download
test-local-stt-download: test-local-stt
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -DASCOLTO_LOCAL_STT_TEST_MANIFEST -DASCOLTO_LOCAL_STT_TEST_HTTPS -Iinclude src/local_stt.c src/database.c src/storage.c src/filesystem.c src/ai_settings.c tests/local_stt_download.c $(STT_TEST_LINK) -o build/local-stt-tests/download
	./build/local-stt-tests/download
