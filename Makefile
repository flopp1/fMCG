# fMCG Makefile (Linux / macOS)
#
# Resolves dependencies through pkg-config, so libraries installed by your
# system package manager are picked up automatically:
#
#   Debian/Ubuntu : sudo apt install build-essential pkg-config libglfw3-dev libarchive-dev libgl1-mesa-dev
#   Fedora        : sudo dnf install gcc-c++ make pkgconf-pkg-config glfw-devel libarchive-devel mesa-libGL-devel
#   Arch          : sudo pacman -S base-devel pkgconf glfw libarchive mesa
#   openSUSE      : sudo zypper install gcc-c++ make pkgconf-pkg-config glfw-devel libarchive-devel Mesa-libGL-devel
#   macOS         : brew install pkg-config glfw libarchive
#
# Dear ImGui is a git submodule at vendor/imgui (fetch it with
# `git submodule update --init`), or point IMGUI_CFLAGS at a system copy:
#
#   make IMGUI_CFLAGS=-I/usr/include/imgui IMGUI_SOURCES=
#
# Targets:
#   make              build ./fMCG_gui (default)
#   make run          build and launch the GUI
#   make debug        build with FMCG_DEBUG=1 (keeps ffmpeg's stderr log in
#                     <name>_fMCG_progress.txt next to the output; release
#                     builds discard it)
#   make test         build and run both test suites (test/)
#   make release      build + test + package dist/fMCG-linux.tar.gz
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra
LDFLAGS  ?=
UNAME    := $(shell uname)
PKG      := pkg-config

# --- OpenGL linkage differs by platform ---------------------------------------
ifeq ($(UNAME),Darwin)
GL_LDLIBS := -framework OpenGL
else
GL_LDLIBS := -lGL
endif

# --- GLFW: try both common pkg-config module names ----------------------------
GLFW_PC := $(shell $(PKG) --exists glfw3 2>/dev/null && echo glfw3)
ifeq ($(GLFW_PC),)
GLFW_PC := $(shell $(PKG) --exists glfw 2>/dev/null && echo glfw)
endif
ifeq ($(GLFW_PC),)
$(warning pkg-config cannot find GLFW. Install the development package:)
$(warning   Debian/Ubuntu: sudo apt install libglfw3-dev)
$(warning   Fedora:        sudo dnf install glfw-devel)
$(warning   Arch:          sudo pacman -S glfw)
$(warning   macOS:         brew install glfw)
GLFW_CFLAGS :=
GLFW_LIBS   := -lglfw
else
GLFW_CFLAGS := $(shell $(PKG) --cflags $(GLFW_PC))
GLFW_LIBS   := $(shell $(PKG) --libs $(GLFW_PC))
endif

# --- libarchive ----------------------------------------------------------------
ifeq ($(shell $(PKG) --exists libarchive 2>/dev/null && echo y),y)
ARCH_CFLAGS := $(shell $(PKG) --cflags libarchive)
ARCH_LIBS   := $(shell $(PKG) --libs libarchive)
else
$(warning pkg-config cannot find libarchive. Install the development package:)
$(warning   Debian/Ubuntu: sudo apt install libarchive-dev)
$(warning   Fedora:        sudo dnf install libarchive-devel)
$(warning   Arch:          sudo pacman -S libarchive)
$(warning   macOS:         brew install libarchive)
ARCH_CFLAGS :=
ARCH_LIBS   := -larchive
endif

# --- Dear ImGui (git submodule at vendor/imgui) ----------------------------------
IMGUI_DIR  ?= vendor/imgui
IMGUI_CFLAGS ?= -I$(IMGUI_DIR)
IMGUI_SOURCES ?= $(IMGUI_DIR)/imgui.cpp \
                 $(IMGUI_DIR)/imgui_draw.cpp \
                 $(IMGUI_DIR)/imgui_tables.cpp \
                 $(IMGUI_DIR)/imgui_widgets.cpp \
                 $(IMGUI_DIR)/imgui_demo.cpp \
                 $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
                 $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

ALL_CFLAGS := $(CXXFLAGS) $(GLFW_CFLAGS) $(ARCH_CFLAGS) $(IMGUI_CFLAGS) -Isrc -Igui
ALL_LIBS   := $(GLFW_LIBS) $(ARCH_LIBS) $(GL_LDLIBS) -lpthread

CORE_SOURCES := src/fmcg_path.cpp src/fmcg_util.cpp src/fmcg_midi.cpp \
                src/fmcg_engine.cpp src/fmcg_format.cpp src/fmcg_fonts.cpp src/fmcg_render.cpp
CORE_OBJECTS := $(CORE_SOURCES:.cpp=.o)
GUI_SOURCES  := gui/app_state.cpp gui/dialogs.cpp gui/jobs.cpp gui/preview.cpp gui/main.cpp \
                gui/colour_edit.cpp gui/settings_store.cpp
GUI_OBJECTS  := $(GUI_SOURCES:.cpp=.o)

# 'all' must stay the FIRST target -- GNU Make treats it as the default.
.PHONY: all run debug test release clean

all: fMCG_gui

debug: CXXFLAGS += -DFMCG_DEBUG=1
debug: fMCG_gui

fMCG_gui: $(GUI_OBJECTS) $(CORE_OBJECTS) $(IMGUI_SOURCES)
	$(CXX) $(ALL_CFLAGS) -o $@ $(GUI_OBJECTS) $(CORE_OBJECTS) $(IMGUI_SOURCES) $(LDFLAGS) $(ALL_LIBS)

src/%.o: src/%.cpp $(wildcard src/*.h) fMCG_core.h
	$(CXX) $(CXXFLAGS) $(ARCH_CFLAGS) -Isrc -I. -c $< -o $@

gui/%.o: gui/%.cpp $(wildcard gui/*.h) $(wildcard src/*.h) fMCG_core.h
	$(CXX) $(ALL_CFLAGS) -c $< -o $@

run: fMCG_gui
	./fMCG_gui

test: test/test_newopts test/test_harness test/test_twopass
	cd test && ./test_newopts && ./test_harness && ./test_twopass test/test.mid

test/test_newopts: test/test_newopts.cpp $(CORE_OBJECTS) $(wildcard src/*.h) fMCG_core.h
	$(CXX) $(CXXFLAGS) -Isrc -I. -o $@ test/test_newopts.cpp $(CORE_OBJECTS) $(ARCH_CFLAGS) $(LDFLAGS) $(ARCH_LIBS) -lpthread

test/test_harness: test/test_harness.cpp $(CORE_OBJECTS) $(wildcard src/*.h) fMCG_core.h
	$(CXX) $(CXXFLAGS) -Isrc -I. -o $@ test/test_harness.cpp $(CORE_OBJECTS) $(ARCH_CFLAGS) $(LDFLAGS) $(ARCH_LIBS) -lpthread

test/test_twopass: test/test_twopass.cpp $(CORE_OBJECTS) $(wildcard src/*.h) fMCG_core.h
	$(CXX) $(CXXFLAGS) -Isrc -I. -o $@ test/test_twopass.cpp $(CORE_OBJECTS) $(ARCH_CFLAGS) $(LDFLAGS) $(ARCH_LIBS) -lpthread

clean:
	rm -rf fMCG_gui $(CORE_OBJECTS) $(GUI_OBJECTS) test/test_newopts test/test_harness dist

# Assemble a ready-to-share release: binary + licences + README.
release: all test
	@mkdir -p dist/fMCG-linux
	cp fMCG_gui LICENSE THIRD_PARTY_LICENSES.txt README.md dist/fMCG-linux/
	tar -czf dist/fMCG-linux.tar.gz -C dist fMCG-linux
	@echo "Release ready: dist/fMCG-linux.tar.gz"
