# win7-tts - build the Windows 7 TTS package.
#
#   make            build everything into runtime/  (same as `make all`)
#   make test       build + run the shim self-test
#   make release    build + zip the two distributables into dist/
#   make upgrade    re-fetch a new sherpa-onnx and rebuild against it
#   make clean      remove build products (keeps runtime/ and models/)
#   make distclean  remove everything generated, including runtime/
#
# Requires: mingw-w64 (x86_64-w64-mingw32-gcc), python3, zip.
# On Windows use build.cmd instead; it drives the same steps.

SHELL      := /bin/bash
CC         := x86_64-w64-mingw32-gcc
PY         := python3
SRC        := src
BUILD      := build
SHERPA     := $(BUILD)/sherpa/unpacked
RUNTIME    := runtime
DIST       := dist
SERPA_BINS := $(SHERPA)/bin/sherpa-onnx-offline-tts.exe \
              $(SHERPA)/lib/sherpa-onnx-c-api.dll \
              $(SHERPA)/lib/onnxruntime.dll

SHIM_SRC   := $(SRC)/shim/win7shim.c $(SRC)/shim/win7shim_fallback.c
COMMON_LIBS:= -lkernel32 -static-libgcc
CAPI_LIB   := -L$(RUNTIME) -lsherpa-onnx-c-api
GUI_LIBS   := $(CAPI_LIB) -lcomctl32 -lwinmm -lshell32 -luuid -lole32

.PHONY: all deps shim say gui diag server runtime test release upgrade clean distclean help FORCE

all: runtime

help:
	@sed -n '2,12p' Makefile | sed 's/^# \{0,1\}//'

deps:
	@echo "checking build dependencies..."
	@command -v $(CC) >/dev/null || { echo "ERROR: $(CC) not found. Install mingw-w64:"; \
	  echo "  apt-get install g++-mingw-w64-x86-64"; exit 1; }
	@command -v $(PY)   >/dev/null || { echo "ERROR: python3 not found"; exit 1; }
	@echo "  mingw-w64: $$($(CC) -dumpversion)"
	@echo "  python   : $$($(PY) --version)"

# ---------------------------------------------------------------- fetch upstream
$(SHERPA)/.complete:
	@echo "==> fetching sherpa-onnx release"
	@$(PY) $(SRC)/shim/fetch_sherpa.py

# ---------------------------------------------------------------- shim
SHIM_TABLE := $(BUILD)/shim/win7shim_table.c
SHIM_ASM   := $(BUILD)/shim/win7shim_thunks.S
SHIM_DEF   := $(BUILD)/shim/win7shim.def

# gen_shim.py writes all three files at once; list them as co-targets so make
# knows they arrive together.
$(SHIM_TABLE) $(SHIM_ASM) $(SHIM_DEF) &: $(SRC)/shim/imports.txt $(SRC)/shim/gen_shim.py
	@echo "==> generating shim table"
	@mkdir -p $(BUILD)/shim
	@$(PY) $(SRC)/shim/gen_shim.py $(SRC)/shim/imports.txt --outdir $(BUILD)/shim

$(RUNTIME)/w7shim.dll: $(SHIM_SRC) $(SRC)/shim/win7shim.h $(SHIM_TABLE) $(SHIM_ASM) $(SHIM_DEF)
	@echo "==> compiling w7shim.dll"
	@mkdir -p $(RUNTIME)
	@$(CC) -shared -O2 -Wall -I$(SRC)/shim -o $@ $(SHIM_SRC) $(SHIM_TABLE) $(SHIM_ASM) $(SHIM_DEF) $(COMMON_LIBS)

shim: $(RUNTIME)/w7shim.dll

# ------------------------------------------------- imports.txt from real binaries
# Never hand-maintained: a new upstream release can import a Windows 8+ function
# nobody redirected, and the failure looks like a mystery rather than a missing
# name in this file.
$(SRC)/shim/imports.txt: $(SHERPA)/.complete $(SRC)/shim/scan_imports.py \
                         $(SRC)/shim/sherpa-version.env
	@echo "==> scanning upstream binaries for imports the shim must provide"
	@$(PY) $(SRC)/shim/scan_imports.py $(SERPA_BINS) --out $@

# ---------------------------------------------------------------- server
SERVER_SRC := $(SRC)/server/tts_server.c $(SRC)/server/http_min.c \
               $(SRC)/server/json_min.c $(SRC)/server/wav_writer.c \
               $(SRC)/engine/tts_engine.c

$(BUILD)/server/ui_html.h: $(SRC)/server/ui/index.html $(SRC)/server/ui/app.js \
                           $(SRC)/server/embed_ui.py
	@mkdir -p $(BUILD)/server
	@$(PY) $(SRC)/server/embed_ui.py $(SRC)/server/ui $@

$(RUNTIME)/tts_server.exe: $(SERVER_SRC) $(BUILD)/server/ui_html.h \
                           $(SRC)/server/*.h $(SRC)/engine/*.h \
                           $(RUNTIME)/sherpa-onnx-c-api.dll
	@echo "==> compiling tts_server.exe"
	@$(CC) -O2 -Wall -mwindows -municode -I$(SHERPA)/include -I$(SRC)/server -I$(SRC)/engine -I$(BUILD)/server \
	      -o $@ $(SERVER_SRC) \
	      $(CAPI_LIB) -lws2_32 -lwinmm -static-libgcc

server: $(RUNTIME)/tts_server.exe

# ---------------------------------------------------------------- front ends
$(RUNTIME)/say.exe: $(SRC)/say/say.c $(RUNTIME)/sherpa-onnx-c-api.dll
	@echo "==> compiling say.exe"
	@$(CC) -O2 -Wall -o $@ $< -I$(SHERPA)/include $(CAPI_LIB)

say: $(RUNTIME)/say.exe

$(RUNTIME)/tts_gui.exe: $(SRC)/gui/gui.c $(RUNTIME)/sherpa-onnx-c-api.dll
	@echo "==> compiling tts_gui.exe"
	@$(CC) -O2 -Wall -mwindows -municode -o $@ $< $(SRC)/engine/tts_engine.c \
	      -I$(SHERPA)/include -I$(SRC)/engine $(GUI_LIBS)

gui: $(RUNTIME)/tts_gui.exe

$(RUNTIME)/diagnose.exe: $(SRC)/diag/diag.c
	@echo "==> compiling diagnose.exe"
	@$(CC) -O2 -Wall -o $@ $<

diag: $(RUNTIME)/diagnose.exe

$(RUNTIME)/.patched: $(RUNTIME)/w7shim.dll $(SHERPA)/.complete
	@echo "==> patching upstream imports"
	@mkdir -p $(RUNTIME)
	@cp -f $(SHERPA)/bin/sherpa-onnx-offline-tts.exe $(RUNTIME)/
	@cp -f $(SHERPA)/lib/onnxruntime.dll $(SHERPA)/lib/onnxruntime_providers_shared.dll \
	       $(SHERPA)/lib/sherpa-onnx-c-api.dll $(SHERPA)/lib/sherpa-onnx-cxx-api.dll $(RUNTIME)/
	@# Patch the COPIES in runtime/, never build/sherpa/unpacked/.  Patching
	@# the downloaded originals would stop the next `scan_imports` from finding
	@# KERNEL32.dll (they would already say w7shim.dll) and silently produce an
	@# empty imports.txt.
	@for f in $(notdir $(SERPA_BINS)); do \
	   $(PY) $(SRC)/shim/patch_pe.py "$(RUNTIME)/$$f" \
	         --inplace --shim $(RUNTIME)/w7shim.dll | sed 's/^/    /'; \
	 done
	@touch $@

runtime: $(RUNTIME)/.patched $(RUNTIME)/say.exe $(RUNTIME)/tts_gui.exe \
         $(RUNTIME)/diagnose.exe $(RUNTIME)/tts_server.exe
	@echo "==> runtime ready in $(RUNTIME)/"

# ---------------------------------------------------------------- test
# The self-test must run against the shim's *fallbacks*, but Wine exports the
# Windows 8+ APIs itself, so a normal build would never exercise them.  Two
# things make the test honest:
#
#   1. its whole KERNEL32 descriptor is redirected to the shim, and
#   2. it resolves each fallback by name through the win7shim_get_fallback hook
#      rather than calling the import, so Wine's own implementation cannot mask
#      a broken fallback.
#
# It imports a few KERNEL32 functions the shipped binaries do not, so a
# superset shim is built for the test; the shipped one stays minimal.
TEST_SHIM := $(BUILD)/shim/w7shim-test.dll

# Always rebuild: the test exe is patched in place, so a stale one would no
# longer import KERNEL32.dll and the scanner/patcher would (correctly) refuse.
$(BUILD)/shim/test_shim.exe: $(SRC)/shim/test/test_shim.c FORCE
	@mkdir -p $(BUILD)/shim
	@rm -f $@
	@$(CC) -O2 -Wall -I$(SRC)/shim -o $@ $<

FORCE:

$(TEST_SHIM): $(SHIM_SRC) $(SRC)/shim/win7shim.h $(BUILD)/shim/test_shim.exe \
              $(SRC)/shim/scan_imports.py $(SRC)/shim/imports.txt
	@echo "==> building test shim (production imports + the test's own)"
	@mkdir -p $(BUILD)/shim-test
	@cp -f $(SRC)/shim/imports.txt $(BUILD)/shim/imports-test.txt
	@$(PY) $(SRC)/shim/scan_imports.py $(BUILD)/shim/test_shim.exe \
	      --out $(BUILD)/shim/imports-test.txt --append
	@$(PY) $(SRC)/shim/gen_shim.py $(BUILD)/shim/imports-test.txt \
	      --outdir $(BUILD)/shim-test >/dev/null
	@$(CC) -shared -O2 -I$(SRC)/shim -o $@ $(SHIM_SRC) \
	      $(BUILD)/shim-test/win7shim_table.c $(BUILD)/shim-test/win7shim_thunks.S \
	      $(BUILD)/shim-test/win7shim.def $(COMMON_LIBS)

$(BUILD)/json_test.exe: $(SRC)/server/test/test_json_min.c $(SRC)/server/json_min.c
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -I$(SRC)/server -o $@ $^ -lm

test: $(BUILD)/shim/test_shim.exe $(TEST_SHIM) $(BUILD)/json_test.exe
	@echo "==> patching the self-test"
	@# The test LoadLibraryA()s "w7shim.dll", so give the superset shim that
	@# name here; it is a superset of the shipped one, so it is equivalent for
	@# every fallback the test exercises.
	@cp -f $(TEST_SHIM) $(BUILD)/shim/w7shim.dll
	@$(PY) $(SRC)/shim/patch_pe.py $(BUILD)/shim/test_shim.exe \
	      --inplace --shim $(BUILD)/shim/w7shim.dll >/dev/null
	@echo "==> running shim self-test (needs wine)"
	@WINEPREFIX=$${WINEPREFIX:-/tmp/wp7} WINEDEBUG=-all \
	  wine $(BUILD)/shim/test_shim.exe 2>/dev/null | tr -d '\000'
	@echo "==> running JSON helper test"
	@WINEPREFIX=$${WINEPREFIX:-/tmp/wp7} WINEDEBUG=-all \
	  wine $(BUILD)/json_test.exe 2>/dev/null | tr -d '\000'
	@# End-to-end over real HTTP.  This is the test that catches a server which
	@# answers 200 while never reading the request body - a failure the unit
	@# tests above cannot see.
	@echo "==> running HTTP API smoke test"
	@$(PY) scripts/api_smoke_test.py --port $${API_TEST_PORT:-8799}

# ---------------------------------------------------------------- release
release: runtime
	@echo "==> packaging $(DIST)/"
	@mkdir -p $(DIST)
	@rm -f $(DIST)/*.zip
	@rm -f $(DIST)/win7-tts.zip
	@# Zip from the parent so the archive contains a single top-level
	@# win7-tts/ folder and extracts tidily on Windows.
	@cd .. && zip -r -q win7-tts/$(DIST)/win7-tts.zip win7-tts \
	    -x 'win7-tts/build/*' 'win7-tts/.git/*' 'win7-tts/dist/*' \
	       'win7-tts/runtime/*.wav' 'win7-tts/runtime/_diag_*' \
	       '*__pycache__*'
	@echo "  $(DIST)/win7-tts.zip"
	@$(PY) scripts/make_slim_zip.py $(DIST)

# ---------------------------------------------------------------- upgrade
upgrade:
	@$(PY) $(SRC)/shim/fetch_sherpa.py --force
	@$(PY) $(SRC)/shim/scan_imports.py $(SERPA_BINS) --out $(SRC)/shim/imports.txt
	@rm -rf $(BUILD)/shim $(RUNTIME)/.patched
	@echo "==> imports.txt regenerated; run 'make' to rebuild"
	@grep -c . $(SRC)/shim/imports.txt | tail -1

# ---------------------------------------------------------------- clean
clean:
	@rm -rf $(BUILD)
	@rm -f $(RUNTIME)/.patched
	@echo "cleaned build products (runtime/ and models/ kept)"

distclean: clean
	@rm -rf $(DIST)
	@rm -f $(RUNTIME)/*.dll $(RUNTIME)/*.exe
	@echo "cleaned runtime/ as well"
