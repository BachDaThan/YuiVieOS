# YuiVieOS — top-level build.
#
# Everything here is plain POSIX + a C++17 compiler.  The Rust workspace
# (kernel-cap, kernel-services/*) is built with cargo (see docs/BUILD.md) and is
# optional for the C++ targets.
#
#   make            build every C++ target into build/bin
#   make test       build and run the whole C++ test suite
#   make check      test + end-to-end smoke runs + static verification
#   make verify     static checks only (needs no toolchain but a C compiler)
#   make install    install into $(PREFIX) (default /usr/local)
#   make kernel     build the bare-metal frame for ARCH (nightly + QEMU image)
#   make clean

CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   ?= -std=gnu11 -O2 -g -Wall -Wextra
CPPFLAGS += -Iinclude -Itests -Ikernel-services -Iuserspace-vi -D_GNU_SOURCE
LDFLAGS  ?=
LDLIBS   ?= -lpthread -lm

BUILD    := build
BIN      := $(BUILD)/bin
OBJ      := $(BUILD)/obj
PREFIX   ?= /usr/local
DESTDIR  ?=

# Tầng 1/2 primitives live in kernel-services, the AI bridge and its reference
# implementations in ai-bridge-yuid, the Vietnamese user space in userspace-vi.
LIB_SRC := \
	kernel-services/crypto.cpp \
	kernel-services/codec.cpp \
	kernel-services/profile.cpp \
	kernel-services/telemetry.cpp \
	kernel-services/ed25519.cpp \
	kernel-services/bundle.cpp \
	kernel-services/power.cpp \
	kernel-services/guard.cpp \
	kernel-services/fsstore.cpp \
	kernel-services/linuxabi.cpp \
	vmm-sandbox/sandbox.cpp \
	pkg-stripper/apppack.cpp \
	userspace-vi/text.cpp \
	userspace-vi/ime.cpp \
	userspace-vi/ui.cpp \
	userspace-vi/fb.cpp \
	userspace-vi/shell.cpp \
	ai-bridge-yuid/reference/graph.cpp \
	ai-bridge-yuid/reference/engine.cpp \
	ai-bridge-yuid/reference/vault.cpp \
	ai-bridge-yuid/daemon/service.cpp \
	ai-bridge-yuid/daemon/server.cpp

LIB_OBJ := $(patsubst %.cpp,$(OBJ)/%.o,$(LIB_SRC))

# Header changes must trigger a rebuild (otherwise a comment/doc fix looks like
# "nothing to be done" and stale objects are silently reused).
HEADERS := $(wildcard include/yui/*.hpp include/yui/*.h) \
           $(wildcard tests/*.hpp) $(wildcard userspace-vi/*.inc)

TESTS := test_crypto test_codec_json test_core test_sandbox test_ime test_daemon test_tools \
         test_telemetry test_fb test_ed25519 test_power test_fsstore test_linuxabi \
         test_redteam

# `kernel` is declared before `all`, so pin the default goal explicitly.
.DEFAULT_GOAL := all

.PHONY: all test check verify install clean rust fmt kernel qemu

# Bare-metal frame: separate toolchain (nightly + rust-src), separate workspace.
# Kept out of `all` so a C++ checkout still builds on a machine without Rust.
ARCH ?= x86_64
kernel:
	./scripts/build-kernel.sh $(ARCH)

qemu:
	./scripts/qemu-smoke.sh $(ARCH) --build

all: $(BIN)/yuid $(BIN)/libyui.a $(BIN)/libyui.so \
     $(BIN)/yui-profiler $(BIN)/yui-trim $(BIN)/yui-shell $(BIN)/yui-splash

$(OBJ)/%.o: %.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(OBJ)/capi.o: ai-bridge-yuid/capi.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BIN)/libyui.a: $(LIB_OBJ) $(OBJ)/capi.o
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BIN)/libyui.so: $(LIB_OBJ) $(OBJ)/capi.o
	@mkdir -p $(dir $@)
	$(CXX) -shared -o $@ $^ $(LDLIBS)

$(BIN)/yuid: $(OBJ)/ai-bridge-yuid/daemon/main.o $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BIN)/yui-profiler: $(OBJ)/tools/profiler/main.o $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BIN)/yui-trim: $(OBJ)/tools/trim/main.o $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BIN)/yui-shell: $(OBJ)/tools/shell/main.o $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BIN)/yui-splash: $(OBJ)/tools/splash/main.o $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ)/test/%: tests/%.cpp $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -DYUI_SOURCE_DIR='"$(CURDIR)"' -o $@ $< $(BIN)/libyui.a $(LDLIBS)

$(BUILD)/test_capi: tests/test_capi.c $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(BIN)/libyui.a -lstdc++ $(LDLIBS)

test: $(addprefix $(OBJ)/test/,$(TESTS)) $(BUILD)/test_capi
	@set -e; for t in $(TESTS); do echo "── $$t"; ./$(OBJ)/test/$$t; done
	@echo "── test_capi (C11)"
	@./$(BUILD)/test_capi

verify:
	@./scripts/verify.sh

check: test verify
	@echo "── smoke: yuid --info"
	@./$(BIN)/yuid --info > /dev/null
	@echo "── smoke: yuid --profile"
	@./$(BIN)/yuid --profile | head -5
	@echo "── smoke: yui-profiler"
	@./$(BIN)/yui-profiler | head -4
	@echo "── smoke: shell script"
	@printf 'thôngtin\nthoát\n' | ./$(BIN)/yui-shell > /dev/null
	@echo "── smoke: splash → PPM"
	@./$(BIN)/yui-splash --width 320 --height 200 --out /tmp/yui-splash.ppm > /dev/null
	@test -s /tmp/yui-splash.ppm
	@echo "all checks passed"

install: all
	install -Dm755 $(BIN)/yuid $(DESTDIR)$(PREFIX)/bin/yuid
	install -Dm755 $(BIN)/yui-profiler $(DESTDIR)$(PREFIX)/bin/yui-profiler
	install -Dm755 $(BIN)/yui-trim $(DESTDIR)$(PREFIX)/bin/yui-trim
	install -Dm755 $(BIN)/yui-shell $(DESTDIR)$(PREFIX)/bin/yui-shell
	install -Dm755 $(BIN)/yui-splash $(DESTDIR)$(PREFIX)/bin/yui-splash
	install -Dm644 $(BIN)/libyui.a $(DESTDIR)$(PREFIX)/lib/libyui.a
	install -Dm755 $(BIN)/libyui.so $(DESTDIR)$(PREFIX)/lib/libyui.so
	install -Dm644 include/yui/yui.h $(DESTDIR)$(PREFIX)/include/yui/yui.h

# Rust workspace at the repository root: kernel-cap (Tầng 1) and the
# kernel-services crates (Tầng 2).  Needs cargo; the C++ targets do not.
rust:
	cargo test --workspace
	cargo build --workspace --release

fmt:
	cargo fmt --all
	cargo clippy --workspace --all-targets

clean:
	rm -rf $(BUILD)
