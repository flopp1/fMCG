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

ALL_CFLAGS := $(CXXFLAGS) $(GLFW_CFLAGS) $(ARCH_CFLAGS) $(IMGUI_CFLAGS)
ALL_LIBS   := $(GLFW_LIBS) $(ARCH_LIBS) $(GL_LDLIBS) -lpthread

# 'all' must stay the FIRST target -- GNU Make treats it as the default.
.PHONY: all run test release clean

all: fMCG_gui

fMCG_gui: fMCG_gui.cpp fMCG_core.h $(IMGUI_SOURCES)
	$(CXX) $(ALL_CFLAGS) -o $@ fMCG_gui.cpp $(IMGUI_SOURCES) $(LDFLAGS) $(ALL_LIBS)

run: fMCG_gui
	./fMCG_gui

test: test/test_newopts test/test_harness
	cd test && ./test_newopts && ./test_harness

test/test_newopts: test/test_newopts.cpp fMCG_core.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_newopts.cpp $(ARCH_CFLAGS) $(LDFLAGS) $(ARCH_LIBS) -lpthread

test/test_harness: test/test_harness.cpp fMCG_core.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_harness.cpp $(ARCH_CFLAGS) $(LDFLAGS) $(ARCH_LIBS) -lpthread

clean:
	rm -rf fMCG_gui test/test_newopts test/test_harness dist

# Assemble a ready-to-share release: binary + licence notices + README.
release: all test
	@mkdir -p dist/fMCG-linux
	cp fMCG_gui THIRD_PARTY_LICENSES.txt README.md dist/fMCG-linux/
	tar -czf dist/fMCG-linux.tar.gz -C dist fMCG-linux
	@echo "Release ready: dist/fMCG-linux.tar.gz" test/test_harness
