# hardcore — build for Windows from WSL (mingw-w64 cross compiler) or MSYS2,
# and for macOS with the Xcode command line tools.
#
# Windows (from WSL, or from a Mac with brew's mingw-w64):
#   make hardcore.exe → hardcore.exe          (the default everywhere but macOS)
#   make test         → unit tests + xterm.js fixtures + headless ConPTY/WSL selftest
#   make bench/termbench.exe → the measuring tool (see bench/RESULTS.md)
#   make install      → copy to %LOCALAPPDATA%\hardcore\ and add Start menu + desktop shortcuts
#
# macOS:
#   make              → hardcore.app (and build/hardcore, the bare binary)
#   make test         → unit tests + xterm.js fixtures + headless forkpty/shell selftest
#   make install      → hardcore.app into ~/Applications

UNAME := $(shell uname -s)

CC      ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LIBS     = -mwindows -municode -ld3d11 -ldxgi -luuid -lgdi32 -luser32 -lkernel32 -lshell32 -static

ifeq ($(origin CC),default)
CC = x86_64-w64-mingw32-gcc
endif

MAC_CC    ?= clang
MAC_FLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-deprecated-declarations \
             -mmacosx-version-min=11.0 -arch arm64 -arch x86_64
MAC_LIBS   = -framework Cocoa -framework OpenGL -lutil
APP        = hardcore.app

.PHONY: all clean selftest install test mac-test

ifeq ($(UNAME),Darwin)
all: $(APP)
test: mac-test
install: $(APP)
	mkdir -p "$(HOME)/Applications"
	rm -rf "$(HOME)/Applications/$(APP)"
	cp -R $(APP) "$(HOME)/Applications/"
	@echo "installed $(HOME)/Applications/$(APP)"
else
all: hardcore.exe
test: win-test
install: win-install
endif

FONT = font/unifont-18.0.01.hex.gz font/hardcore-extra.hex

build/font.bin: $(FONT) tools/gen_font.py
	@mkdir -p build
	python3 tools/gen_font.py $@ $(FONT)

build/width.inc: font/ucd/EastAsianWidth.txt font/ucd/DerivedGeneralCategory.txt tools/gen_width.py
	@mkdir -p build
	python3 tools/gen_width.py font/ucd > $@

font/hardcore-extra.hex: tools/draw_extra.py
	python3 $< > $@

build/hardcore.ico: tools/gen_icon.py
	@mkdir -p build
	python3 tools/gen_icon.py $@

build/res.o: hardcore.rc build/hardcore.ico
	$(WINDRES) $< -O coff -o $@

CONPTY_VER = 1.25.260930003

hardcore.exe: hardcore.c build/font.bin build/width.inc build/res.o vendor/conpty/conpty.dll vendor/conpty/OpenConsole.exe
	$(CC) $(CFLAGS) -DFONT_BIN='"build/font.bin"' -DCONPTY_VER='L"$(CONPTY_VER)"' -DCONPTY_VER_A='"$(CONPTY_VER)"' \
		-o $@ hardcore.c build/res.o $(LIBS)

win-test: hardcore.exe
	./hardcore.exe --unit "$$(wslpath -w build/unit.txt)"; rc=$$?; tail -1 build/unit.txt; [ $$rc = 0 ]
	python3 tests/run_xtermjs.py
	$(MAKE) --no-print-directory selftest

bench/termbench.exe: bench/termbench.c
	$(CC) -O2 -Wall -o $@ $< -ld3d11 -ldxgi -lgdi32 -luser32 -luuid

selftest: hardcore.exe
	./hardcore.exe --selftest selftest.txt; rc=$$?; cat selftest.txt; exit $$rc

win-install: hardcore.exe
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/install.ps1 -Exe "$$(wslpath -w hardcore.exe 2>/dev/null || echo hardcore.exe)"

# ── macOS ──────────────────────────────────────────────────────────────

build/hardcore: hardcore.c hardcore_mac.m build/font.bin build/width.inc
	$(MAC_CC) $(MAC_FLAGS) -x objective-c -DFONT_BIN='"build/font.bin"' -o $@ hardcore.c $(MAC_LIBS)

build/hardcore.icns: tools/gen_icon.py
	@mkdir -p build
	rm -rf build/hardcore.iconset
	python3 tools/gen_icon.py build/hardcore.iconset
	iconutil -c icns -o $@ build/hardcore.iconset

$(APP): build/hardcore build/hardcore.icns tools/Info.plist
	rm -rf $@
	mkdir -p $@/Contents/MacOS $@/Contents/Resources
	cp build/hardcore $@/Contents/MacOS/hardcore
	cp build/hardcore.icns $@/Contents/Resources/hardcore.icns
	sed 's/@VERSION@/$(shell sed -n 's/^#define HARDCORE_VERSION "\(.*\)"/\1/p' hardcore.c)/' tools/Info.plist > $@/Contents/Info.plist
	codesign --force --sign - $@ 2>/dev/null || true

mac-test: build/hardcore
	build/hardcore --unit build/unit.txt; rc=$$?; tail -1 build/unit.txt; [ $$rc = 0 ]
	python3 tests/run_xtermjs.py
	build/hardcore --selftest build/selftest.txt; rc=$$?; cat build/selftest.txt; exit $$rc

clean:
	rm -rf build hardcore.exe selftest.txt $(APP)
