# Compiler and flags
CC := gcc
CFLAGS := -O3 -march=native -mtune=native -Wall -Wextra -I./include
LIBS := -lpthread -lm -lfftw3f
RTL_LIBS := -lrtlsdr

# Directories: flat src/; one entry point and one device per input kind
SRC_DIR := src
OBJ_DIR := obj
BIN_DIR := bin

# Profiling build (make profile): production codegen + debug symbols for
# valgrind/callgrind, in a separate obj dir and *.prof binaries
ifeq ($(PROFILE),1)
CFLAGS += -g -fno-omit-frame-pointer
OBJ_DIR := obj/prof
TARGET_SUFFIX := .prof
endif

# Sources: the entry points and device backends are per-binary, everything
# else is shared core
ENTRY_SRC := $(SRC_DIR)/main_file.c $(SRC_DIR)/main_rtl.c
DEV_SRC := $(SRC_DIR)/device_file.c $(SRC_DIR)/device_rtl.c
CORE_SRC := $(filter-out $(ENTRY_SRC) $(DEV_SRC), $(wildcard $(SRC_DIR)/*.c))
CORE_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CORE_SRC))

FILE_BIN := $(BIN_DIR)/file_lora$(TARGET_SUFFIX)
RTL_BIN := $(BIN_DIR)/rtl_lora$(TARGET_SUFFIX)

# All C sources + compile database path (make compdb regenerates it)
SOURCES := $(wildcard $(SRC_DIR)/*.c)
COMPDB := compile_commands.json

.PHONY: all
all: $(FILE_BIN) $(RTL_BIN)

# Create necessary directories
$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

# Compile every source (shared core, entry points, device backends)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Link the file binary: shared core + its entry point + its device
$(FILE_BIN): $(CORE_OBJ) $(OBJ_DIR)/main_file.o $(OBJ_DIR)/device_file.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "✓ Build successful: $@"

# Link the RTL binary: shared core + its entry point + its device
$(RTL_BIN): $(CORE_OBJ) $(OBJ_DIR)/main_rtl.o $(OBJ_DIR)/device_rtl.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(RTL_LIBS)
	@echo "✓ Build successful: $@"

# Profiling builds (valgrind/callgrind); do not touch obj/ or bin/ builds
.PHONY: profile
profile:
	$(MAKE) PROFILE=1 all

# Compile database for editor tooling (clangd finds it automatically; point
# cpptools at it via C_Cpp.default.compileCommands). Regenerate after adding
# sources or changing CFLAGS, then restart the language server.
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

# Clean build artifacts
.PHONY: clean
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "✓ Clean complete"

# C/H style via clang-format (.clang-format at repo root) + Python style for
# tools/ via ruff. fmt-check is the gate counterpart — it exits nonzero, so it
# can sit in a pre-commit hook.
PY_SOURCES := $(wildcard tools/*.py)
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

# API docs (Doxyfile at repo root; output in docs/doxygen/html)
.PHONY: doc
doc:
	@command -v doxygen >/dev/null 2>&1 || { \
		echo "ERROR: doxygen not installed (Arch: sudo pacman -S doxygen)"; exit 1; }
	doxygen Doxyfile
	@echo "✓ API docs: docs/doxygen/html/index.html"

# Help target
.PHONY: help
help:
	@echo "rxx_lora Makefile Targets"
	@echo "========================"
	@echo "  all         - Build bin/file_lora (default)"
	@echo "  profile     - Build *.prof binaries with debug symbols (valgrind/callgrind)"
	@echo "  compdb      - Regenerate compile_commands.json (editor IntelliSense)"
	@echo "  clean       - Remove build artifacts (obj/, bin/)"
	@echo "  doc         - Generate API docs with doxygen"
	@echo "  fmt         - Format src/*.c, include/*.h (clang-format) and tools/*.py (ruff)"
	@echo "  fmt-check   - Check tools/*.py formatting/lint (gate, nonzero on findings)"
	@echo ""
	@echo "Usage:"
	@echo "  make                # Build bin/file_lora"
	@echo "  make profile        # Profiling build"
	@echo "  make clean          # Clean build artifacts"
