# Isiliel — interpretador/compilador de linguagem de script.
# Embarcado: sandbox, API de embutimento, OOP, graficos 2D e REPL.
# Autores: EEPS e MiMo-V2.6 | Versao: 1.0 | Data: 2026-11-08
# Este arquivo: build (lib + runner, release, embedded, bench, test).
CXX      ?= g++
CXXFLAGS ?= -Wall -Wextra -Wpedantic -g3 -O0 -std=c++17 -Wno-unused-parameter
AR       ?= ar
# Flags extras por-invocacao (make release/embedded passam -O2/-Os aqui).
# Vem DEPOIS de CXXFLAGS: o ultimo -O/-DNDEBUG do gcc vence.
EXTRA    ?=

BUILD    := build
LIB      := $(BUILD)/libisiliel.a
BIN      := $(BUILD)/isirun
API_TEST := $(BUILD)/api_test
FUZZ_BIN := $(BUILD)/fuzz

# --- backend grafico raylib (opcional) ---------------------------------------
# Raylib instalado neste projeto (third_party/raylib-install, sem sudo) fica
# automaticamente no pkg-config quando existir.
# (PC_ENV: o export do make nao alcanca os $(shell), entao passamos por comando)
PC_ENV :=
ifneq ($(wildcard third_party/raylib-install/lib/pkgconfig),)
  PC_ENV := PKG_CONFIG_PATH=$(CURDIR)/third_party/raylib-install/lib/pkgconfig:$(PKG_CONFIG_PATH)
  export PKG_CONFIG_PATH := $(CURDIR)/third_party/raylib-install/lib/pkgconfig:$(PKG_CONFIG_PATH)
endif

# USE_RAYLIB=auto (padrao): usa raylib se pkg-config achar
# USE_RAYLIB=1: força o uso (erro se nao achar); USE_RAYLIB=0: nunca usa
USE_RAYLIB ?= auto
ifeq ($(USE_RAYLIB),auto)
  HAS_RAYLIB := $(shell $(PC_ENV) pkg-config --exists raylib 2>/dev/null && echo 1)
else ifeq ($(USE_RAYLIB),1)
  HAS_RAYLIB := 1
else
  HAS_RAYLIB :=
endif

GRAPHICS_DEFS :=
GRAPHICS_LIBS :=
ifeq ($(HAS_RAYLIB),1)
  GRAPHICS_DEFS := -DHAVE_RAYLIB $(shell $(PC_ENV) pkg-config --cflags raylib)
  GRAPHICS_LIBS := $(shell $(PC_ENV) pkg-config --libs raylib)
  ifneq ($(shell $(PC_ENV) pkg-config --exists raylib 2>/dev/null && echo 1),1)
    $(error USE_RAYLIB=1 mas raylib nao encontrado via pkg-config)
  endif
endif
CXXFLAGS += $(GRAPHICS_DEFS)

LIB_SRCS := symbols.cpp lexer.cpp compiler.cpp vm.cpp interp.cpp fileio.cpp \
            sound.cpp sound_raylib.cpp patterns.cpp threads.cpp modules.cpp sha256.cpp \
            graphics/graphics_core.cpp graphics/graphics_soft.cpp \
            graphics/graphics_builtins.cpp graphics/graphics_raylib.cpp
LIB_OBJS := $(LIB_SRCS:%.cpp=$(BUILD)/%.o)

HDRS := $(wildcard *.h) $(wildcard graphics/*.h)

.PHONY: all clean test info-graphics release embedded bench asan fuzz

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp $(HDRS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(EXTRA) -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

# dlopen/dlsym dos modulos `.mod` (item 16): -ldl no Linux; no macOS
# (libSystem) nao ha libdl separada
UNAME_S := $(shell uname -s | tr '[:upper:]' '[:lower:]')
UNAME_M := $(shell uname -m)
ifneq ($(UNAME_S),darwin)
  SYSTEM_LIBS := -ldl
else
  SYSTEM_LIBS :=
endif

$(BIN): run.cpp $(LIB) $(HDRS)
	$(CXX) $(CXXFLAGS) $(EXTRA) run.cpp $(LIB) $(GRAPHICS_LIBS) $(SYSTEM_LIBS) -o $@

# Testes da API de embutimento (E8): interp_run/sandbox/register_fn/reset
$(API_TEST): tests/api_test.cpp $(LIB) $(HDRS)
	$(CXX) $(CXXFLAGS) $(EXTRA) tests/api_test.cpp $(LIB) $(GRAPHICS_LIBS) $(SYSTEM_LIBS) -o $@

# E9/neg20: fuzz deterministico (tests/fuzz.cpp); ligado a lib do interp.
$(FUZZ_BIN): tests/fuzz.cpp $(LIB) $(HDRS)
	$(CXX) $(CXXFLAGS) $(EXTRA) -I. tests/fuzz.cpp $(LIB) $(GRAPHICS_LIBS) $(SYSTEM_LIBS) -o $@

# --- Builds especiais (E7/neg14) --------------------------------------------
# O make nao rastreia mudancas de flags: clean antes para reconstruir do zero.
# release:    -O2 -DNDEBUG -flto  (medicao de desempenho; LTO inlina
#             sym_get e demais chamadas entre unidades)
# embedded:   -Os -DNDEBUG sem raylib (alvo embarcado, tamanho minimo)
release:
	$(MAKE) clean
	$(MAKE) EXTRA="-O2 -DNDEBUG -flto" all

embedded:
	$(MAKE) clean
	$(MAKE) EXTRA="-Os -DNDEBUG -flto" USE_RAYLIB=0 all

# E9/neg20: suite completa sob AddressSanitizer + UndefinedBehaviorSanitizer.
# Leak detection desligada: o processo guarda simbolos/arena ate' o exit por
# design (single-state); o alvo procura erros de memoria/UB, nao leaks de fim
# de processo. Sem raylib (headless, igual ao CI).
asan:
	$(MAKE) clean
	$(MAKE) EXTRA="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        USE_RAYLIB=0 all $(API_TEST) $(FIXTURE_SO)
	ASAN_OPTIONS=detect_leaks=0 bash tests/run_tests.sh

# E9/neg20: fuzz deterministico das sementes em scripts/ (ASAN/UBSAN).
FUZZ_ITERS ?= 2000
fuzz:
	$(MAKE) clean
	$(MAKE) EXTRA="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        USE_RAYLIB=0 $(FUZZ_BIN)
	ASAN_OPTIONS=detect_leaks=0 ./$(FUZZ_BIN) $(FUZZ_ITERS)

# Mostra qual backend grafico esta ativo neste build
info-graphics:
	@echo "USE_RAYLIB=$(USE_RAYLIB) -> raylib: $(if $(HAS_RAYLIB),sim,nao)"

# --- bench: host Lua 5.4 x Isiliel (bench/host_bench.cpp) ------------------
# Lua 5.4.6 instalado em third_party/lua5.4 (extraido dos pacotes .deb,
# sem sudo). Como release/embedded, clean antes: make nao rastreia flags.
LUA_DIR := third_party/lua5.4/usr
LUA_INC := -I$(LUA_DIR)/include/lua5.4 -I$(LUA_DIR)/include/x86_64-linux-gnu
LUA_LIB := $(LUA_DIR)/lib/x86_64-linux-gnu/liblua5.4.a

$(BUILD)/host_bench: bench/host_bench.cpp $(LIB) $(HDRS)
	$(CXX) $(CXXFLAGS) $(EXTRA) $(LUA_INC) -I. bench/host_bench.cpp $(LIB) $(GRAPHICS_LIBS) $(SYSTEM_LIBS) $(LUA_LIB) -lm -o $@

bench:
	$(MAKE) clean
	$(MAKE) EXTRA="-O2 -DNDEBUG -flto" $(BUILD)/host_bench
	./$(BUILD)/host_bench

clean:
	rm -rf $(BUILD)
	rm -rf scripts/modules/lib

# Biblioteca de teste dos modulos `.mod` (item 16): o manifesto em
# scripts/modules/motor_controller.mod aponta para este caminho relativo
FIXTURE_DIR := scripts/modules/lib/$(UNAME_S)/$(UNAME_M)
FIXTURE_SO  := $(FIXTURE_DIR)/libmotor_controller.so

$(FIXTURE_SO): scripts/modules/motor_controller.c
	@mkdir -p $(dir $@)
	$(CC) -shared -fPIC -o $@ $<

test: $(BIN) $(API_TEST) $(FIXTURE_SO)
	@bash tests/run_tests.sh
