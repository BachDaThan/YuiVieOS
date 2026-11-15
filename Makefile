# YuiVieOS — top-level build.
#
# Everything here is plain POSIX + a C++17 compiler.  Rust crates under crates/
# are built with cargo (see docs/BUILD.md) and are optional for the C++ targets.
#
#   make            build every C++ target into build/bin
#   make test       build and run the whole C++ test suite
#   make check      test + a few end-to-end smoke runs
#   make install    install into $(PREFIX) (default /usr/local)
#   make clean

CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   ?= -std=gnu11 -O2 -g -Wall -Wextra
CPPFLAGS += -Icore/include -Icore/tests -Icore/src/kernel -D_GNU_SOURCE
LDFLAGS  ?=
LDLIBS   ?= -lpthread -lm

BUILD    := build
BIN      := $(BUILD)/bin
OBJ      := $(BUILD)/obj
PREFIX   ?= /usr/local
DESTDIR  ?=

LIB_SRC := \
	core/src/crypto/crypto.cpp \
	core/src/crypto/vault.cpp \
	core/src/codec.cpp \
	core/src/kernel/graph.cpp \
	core/src/kernel/engine.cpp \
	core/src/kernel/text.cpp \
	core/src/kernel/ime.cpp \
	core/src/kernel/profile.cpp \
	core/src/kernel/apppack.cpp \
	core/src/kernel/ui.cpp \
	core/src/sandbox/sandbox.cpp \
	core/src/daemon/service.cpp \
	core/src/daemon/server.cpp \
	core/src/daemon/shell.cpp

LIB_OBJ := $(patsubst %.cpp,$(OBJ)/%.o,$(LIB_SRC))

# Header changes must trigger a rebuild (otherwise a comment/doc fix looks like
# "nothing to be done" and stale objects are silently reused).
HEADERS := $(wildcard core/include/yui/*.hpp core/include/yui/*.h) \
           $(wildcard core/tests/*.hpp) $(wildcard core/src/kernel/*.inc)

TESTS := test_crypto test_codec_json test_core test_sandbox test_ime test_daemon test_tools

.PHONY: all test check install clean rust fmt

all: $(BIN)/yuid $(BIN)/libyui.a $(BIN)/libyui.so \
     $(BIN)/yui-profiler $(BIN)/yui-trim $(BIN)/yui-shell

$(OBJ)/%.o: %.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(OBJ)/capi.o: core/src/capi/capi.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BIN)/libyui.a: $(LIB_OBJ) $(OBJ)/capi.o
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BIN)/libyui.so: $(LIB_OBJ) $(OBJ)/capi.o
	@mkdir -p $(dir $@)
	$(CXX) -shared -o $@ $^ $(LDLIBS)

$(BIN)/yuid: $(OBJ)/core/src/daemon/main.o $(BIN)/libyui.a
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

$(OBJ)/test/%: core/tests/%.cpp $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -DYUI_SOURCE_DIR='"$(CURDIR)"' -o $@ $< $(BIN)/libyui.a $(LDLIBS)

$(BUILD)/test_capi: core/tests/test_capi.c $(BIN)/libyui.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Icore/include -o $@ $< $(BIN)/libyui.a -lstdc++ $(LDLIBS)

test: $(addprefix $(OBJ)/test/,$(TESTS)) $(BUILD)/test_capi
	@set -e; for t in $(TESTS); do echo "── $$t"; ./$(OBJ)/test/$$t; done
	@echo "── test_capi (C11)"
	@./$(BUILD)/test_capi

check: test
	@echo "── smoke: yuid --info"
	@./$(BIN)/yuid --info > /dev/null
	@echo "── smoke: yuid --profile"
	@./$(BIN)/yuid --profile | head -5
	@echo "── smoke: yui-profiler"
	@./$(BIN)/yui-profiler | head -4
	@echo "── smoke: shell script"
	@printf 'thôngtin\nthoát\n' | ./$(BIN)/yui-shell > /dev/null
	@echo "all checks passed"

install: all
	install -Dm755 $(BIN)/yuid $(DESTDIR)$(PREFIX)/bin/yuid
	install -Dm755 $(BIN)/yui-profiler $(DESTDIR)$(PREFIX)/bin/yui-profiler
	install -Dm755 $(BIN)/yui-trim $(DESTDIR)$(PREFIX)/bin/yui-trim
	install -Dm755 $(BIN)/yui-shell $(DESTDIR)$(PREFIX)/bin/yui-shell
	install -Dm644 $(BIN)/libyui.a $(DESTDIR)$(PREFIX)/lib/libyui.a
	install -Dm755 $(BIN)/libyui.so $(DESTDIR)$(PREFIX)/lib/libyui.so
	install -Dm644 core/include/yui/yui.h $(DESTDIR)$(PREFIX)/include/yui/yui.h

rust:
	cargo test --manifest-path crates/yui-cap/Cargo.toml
	cargo test --manifest-path crates/yui-sched/Cargo.toml
	cargo test --manifest-path crates/yui-zram/Cargo.toml

clean:
	rm -rf $(BUILD)
