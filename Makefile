CC = gcc
CFLAGS = -Wall -Wextra -O2
CFLAGS_USB = $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo "-I/opt/homebrew/include")
LDFLAGS_USB = $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo "-L/opt/homebrew/lib -lusb-1.0")

# Platform detection
UNAME_S := $(shell uname -s)

# Source files
CORE_SRC = src/flashmd_core.c
CLI_SRC = src/flashmd_cli.c
QT_SRC = src/flashmd_qt.cpp

# Include paths
INCLUDES = -Isrc

# Qt configuration
ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew - try Qt6 first, then Qt5
    QT_MOC = $(shell find /opt/homebrew/Cellar/qtbase -name "moc" -type f 2>/dev/null | head -1)
    QT_RCC = $(shell find /opt/homebrew/Cellar/qtbase -name "rcc" -type f 2>/dev/null | head -1)
    QT_CFLAGS = $(shell pkg-config --cflags Qt6Widgets 2>/dev/null || pkg-config --cflags Qt5Widgets 2>/dev/null)
    QT_LDFLAGS = $(shell pkg-config --libs Qt6Widgets 2>/dev/null || pkg-config --libs Qt5Widgets 2>/dev/null)
else
    # Linux - try Qt5 first (more common), then Qt6
    QT_CFLAGS = $(shell pkg-config --cflags Qt5Widgets 2>/dev/null || pkg-config --cflags Qt6Widgets 2>/dev/null)
    QT_LDFLAGS = $(shell pkg-config --libs Qt5Widgets 2>/dev/null || pkg-config --libs Qt6Widgets 2>/dev/null)
    QT_MOC = $(shell which moc-qt5 2>/dev/null || which moc 2>/dev/null || echo "moc")
    QT_RCC = $(shell which rcc-qt5 2>/dev/null || which rcc 2>/dev/null || echo "rcc")
endif

# Targets
CLI_TARGET = flashmd
QT_TARGET = flashmd-gui

.PHONY: all cli gui clean help

# Default: build CLI (backward compatible)
all: cli gui

# CLI build
cli: $(CLI_TARGET)

$(CLI_TARGET): $(CORE_SRC) $(CLI_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) $(INCLUDES) -o $@ $^ $(LDFLAGS_USB)

# Qt GUI build
gui: $(QT_TARGET)

src/moc_flashmd_qt.cpp: $(QT_SRC)
	$(QT_MOC) $(QT_SRC) -o src/moc_flashmd_qt.cpp

src/qrc_resources.cpp: res/resources.qrc res/opensans.ttf res/mono.ttf
	$(QT_RCC) res/resources.qrc -o src/qrc_resources.cpp

src/flashmd_core_qt.o: $(CORE_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) $(INCLUDES) -c -o $@ $(CORE_SRC)

$(QT_TARGET): src/flashmd_core_qt.o $(QT_SRC) src/moc_flashmd_qt.cpp src/qrc_resources.cpp
	g++ -std=c++17 $(CFLAGS) $(CFLAGS_USB) $(QT_CFLAGS) $(INCLUDES) -fPIC -o $@ src/flashmd_core_qt.o $(QT_SRC) src/qrc_resources.cpp $(LDFLAGS_USB) $(QT_LDFLAGS)

clean:
	rm -f $(CLI_TARGET) $(QT_TARGET) src/moc_flashmd_qt.cpp src/flashmd_core_qt.o src/qrc_resources.cpp

help:
	@echo "FlashMD Build Targets:"
	@echo "  make cli     - Build command-line version (default)"
	@echo "  make gui  - Build Qt GUI version (recommended)"
	@echo "  make clean   - Remove built binaries"
	@echo ""
	@echo "Dependencies:"
	@echo "  CLI:    libusb-1.0"
	@echo "  gui: libusb-1.0, Qt5"
	@echo ""
	@echo "Install dependencies:"
	@echo "  macOS:  brew install libusb qt@5"
	@echo "  Linux:  sudo apt install libusb-1.0-0-dev qtbase5-dev"
	@echo ""
	@echo "Running on Linux:"
	@echo "  sudo ./flashmd-gui"
