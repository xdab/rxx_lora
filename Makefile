CC := gcc
CFLAGS := -O3 -march=native -mtune=native -Wall -Wextra -I./include
LIBS := -lpthread -lm -lfftw3f
RTL_LIBS := -lrtlsdr
RSP_LIBS := -lsdrplay_api

SRC_DIR := src
OBJ_DIR := obj
BIN_DIR := bin

# make profile: release codegen + debug info (valgrind/callgrind), obj/prof, *.prof
ifeq ($(PROFILE),1)
CFLAGS += -g -fno-omit-frame-pointer
OBJ_DIR := obj/prof
TARGET_SUFFIX := .prof
endif

# make cov: -O0 gcov build for branch/line coverage maps, obj/cov, *.cov
ifeq ($(COVERAGE),1)
CFLAGS += -O0 -g --coverage
OBJ_DIR := obj/cov
TARGET_SUFFIX := .cov
endif

# Entry points and device backends are per-binary; everything else is shared core
ENTRY_SRC := $(SRC_DIR)/main_file.c $(SRC_DIR)/main_rtl.c $(SRC_DIR)/main_rsp.c
DEV_SRC := $(SRC_DIR)/device_file.c $(SRC_DIR)/device_rtl.c $(SRC_DIR)/device_rsp.c
CORE_SRC := $(filter-out $(ENTRY_SRC) $(DEV_SRC), $(wildcard $(SRC_DIR)/*.c))
CORE_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CORE_SRC))

FILE_BIN := $(BIN_DIR)/file_lora$(TARGET_SUFFIX)
RTL_BIN := $(BIN_DIR)/rtl_lora$(TARGET_SUFFIX)
RSP_BIN := $(BIN_DIR)/rsp_lora$(TARGET_SUFFIX)

SOURCES := $(wildcard $(SRC_DIR)/*.c)
COMPDB := compile_commands.json

# file needs no SDR library; rtl needs librtlsdr only; rsp the SDRplay API only
.PHONY: all file rtl rsp
all: file rtl rsp
file: $(FILE_BIN)
rtl: $(RTL_BIN)
rsp: $(RSP_BIN)

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Link: shared core + own entry point + own device + own library
$(FILE_BIN): $(CORE_OBJ) $(OBJ_DIR)/main_file.o $(OBJ_DIR)/device_file.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "✓ Build successful: $@"

$(RTL_BIN): $(CORE_OBJ) $(OBJ_DIR)/main_rtl.o $(OBJ_DIR)/device_rtl.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(RTL_LIBS)
	@echo "✓ Build successful: $@"

$(RSP_BIN): $(CORE_OBJ) $(OBJ_DIR)/main_rsp.o $(OBJ_DIR)/device_rsp.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(RSP_LIBS)
	@echo "✓ Build successful: $@"

# PREFIX/DESTDIR follow GNU conventions (DESTDIR stages a package root)
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

.PHONY: install
install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(FILE_BIN) $(RTL_BIN) $(RSP_BIN) $(DESTDIR)$(BINDIR)
	@[ -z '$(TARGET_SUFFIX)' ] || for b in file_lora rtl_lora rsp_lora; do \
		mv $(DESTDIR)$(BINDIR)/$$b$(TARGET_SUFFIX) $(DESTDIR)$(BINDIR)/$$b; \
	done
	@echo "✓ Installed to $(DESTDIR)$(BINDIR)"

.PHONY: profile
profile:
	$(MAKE) PROFILE=1 all

.PHONY: cov
cov:
	$(MAKE) COVERAGE=1 file

# clangd reads compile_commands.json; regenerate after adding sources or
# changing CFLAGS, then restart the language server
.PHONY: compdb
compdb:
	@: > $(COMPDB); echo '[' >> $(COMPDB)
	@for src in $(SOURCES); do \
		q='\"'; \
		{ printf '{"directory": "%s", "file": "%s", "arguments": ["%s"' "$(CURDIR)" "$$src" "$(CC)"; \
		  for fl in $(CFLAGS); do printf ', "%s"' "$$fl"; done; \
		  printf ', "-c", "%s"]},\n' "$$src"; } >> $(COMPDB); \
	done
	@sed -i '$$ s/},$$/}/' $(COMPDB)
	@echo ']' >> $(COMPDB)
	@echo "✓ $(COMPDB): $(words $(SOURCES)) sources"

-include $(shell find $(OBJ_DIR) -name '*.d' 2>/dev/null)

.PHONY: clean
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "✓ Clean complete"

# clang-format for C/H, ruff for tools/; fmt-check exits nonzero (gate twin)
PY_SOURCES := $(wildcard tools/*.py) $(wildcard tools/prof/*.py)
C_SOURCES := $(wildcard $(SRC_DIR)/*.c) $(wildcard include/*.h)

.PHONY: fmt
fmt:
	@command -v clang-format >/dev/null 2>&1 || { \
		echo "ERROR: clang-format not installed (Arch: sudo pacman -S clang)"; exit 1; }
	@command -v ruff >/dev/null 2>&1 || { \
		echo "ERROR: ruff not installed (Arch: sudo pacman -S ruff)"; exit 1; }
	clang-format -i $(C_SOURCES)
	ruff format $(PY_SOURCES)
	ruff check --fix $(PY_SOURCES)

.PHONY: fmt-check
fmt-check:
	@command -v clang-format >/dev/null 2>&1 || { \
		echo "ERROR: clang-format not installed (Arch: sudo pacman -S clang)"; exit 1; }
	@command -v ruff >/dev/null 2>&1 || { \
		echo "ERROR: ruff not installed (Arch: sudo pacman -S ruff)"; exit 1; }
	clang-format --dry-run --Werror $(C_SOURCES)
	ruff format --check $(PY_SOURCES)
	ruff check $(PY_SOURCES)

# clang-tidy gate (checks in .clang-tidy). The RSP pair is out of the gate:
# CI lacks the SDRplay API header (compdb still lists the files for clangd).
TIDY_SRCS := $(filter-out $(SRC_DIR)/main_rsp.c $(SRC_DIR)/device_rsp.c, $(SOURCES))

.PHONY: tidy
tidy: compdb
	@command -v clang-tidy >/dev/null 2>&1 || { \
		echo "ERROR: clang-tidy not installed (Arch: sudo pacman -S clang-tidy)"; exit 1; }
	clang-tidy -p . $(TIDY_SRCS) --warnings-as-errors=*

# Doxyfile at repo root; output in docs/doxygen/html
.PHONY: doc
doc:
	@command -v doxygen >/dev/null 2>&1 || { \
		echo "ERROR: doxygen not installed (Arch: sudo pacman -S doxygen)"; exit 1; }
	doxygen Doxyfile
	@echo "✓ API docs: docs/doxygen/html/index.html"

.PHONY: help
help:
	@echo "rxx_lora Makefile Targets"
	@echo "========================"
	@echo "  all         - Build bin/file_lora, bin/rtl_lora, bin/rsp_lora (default)"
	@echo "  file        - Build bin/file_lora only (no SDR library needed)"
	@echo "  rtl         - Build bin/rtl_lora only (needs librtlsdr, not the SDRplay API)"
	@echo "  rsp         - Build bin/rsp_lora only (needs the SDRplay API, not librtlsdr)"
	@echo "  install     - Install all three binaries to $(BINDIR)"
	@echo "  profile     - Build *.prof binaries with debug symbols (valgrind/callgrind)"
	@echo "  compdb      - Regenerate compile_commands.json (editor IntelliSense)"
	@echo "  clean       - Remove build artifacts (obj/, bin/)"
	@echo "  doc         - Generate API docs with doxygen"
	@echo "  fmt         - Format src/*.c, include/*.h (clang-format) and tools/*.py (ruff)"
	@echo "  fmt-check   - Check tools/*.py formatting/lint (gate, nonzero on findings)"
	@echo "  tidy        - Static analysis gate over src/*.c (clang-tidy, .clang-tidy)"
	@echo ""
	@echo "Usage:"
	@echo "  make                # Build all three binaries"
	@echo "  make file|rtl|rsp   # Build one binary"
	@echo "  make profile        # Profiling build"
	@echo "  make clean          # Clean build artifacts"
