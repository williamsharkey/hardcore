# hardcore — build for Windows from x86-64 WSL (mingw-w64 and Linux C compilers),
# and for macOS with the Xcode command line tools.
#
# Windows (cross-builds also need LINUX_CC targeting x86-64 Linux with static libc):
#   make hardcore.exe → hardcore.exe          (the default everywhere but macOS)
#   make test         → unit tests + xterm.js fixtures + ConPTY/WSL and slurp tests
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
LIBS     = -mwindows -municode -ld3d11 -ldxgi -luuid -lgdi32 -luser32 -lkernel32 -lshell32 -lwinhttp -lbcrypt -static

ifeq ($(origin CC),default)
CC = x86_64-w64-mingw32-gcc
endif

MAC_CC    ?= clang
MAC_FLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-deprecated-declarations \
             -mmacosx-version-min=11.0 -arch arm64 -arch x86_64
MAC_LIBS   = -framework Cocoa -framework Metal -framework QuartzCore -framework WebKit -framework ImageIO -lutil
APP        = hardcore.app

.PHONY: all clean selftest install test mac-test slurp-test

ifeq ($(UNAME),Darwin)
all: $(APP)
test: mac-test
install: $(APP)
	python3 tools/install_mac.py "$(APP)" "$(HOME)/Applications/$(APP)" "$(HOME)/.local/bin"
else
all: hardcore.exe
test: win-test
install: win-install
endif

FONT = font/unifont-18.0.01.hex.gz font/hardcore-extra.hex

build/font.bin: $(FONT) tools/gen_font.py
	@mkdir -p build
	python3 tools/gen_font.py $@ $(FONT)

# the other fonts for the settings (each one's own glyphs) and their list
FONTS_SRC = font/terminus/ter-u24n.bdf.gz font/terminus/ter-u28n.bdf.gz font/terminus/ter-u32n.bdf.gz \
            font/tamzen/Tamzen8x15r.bdf.gz font/misc-fixed/7x13.bdf.gz font/sony/12x24.bdf.gz font/etl/lt1-24-etl.bdf.gz

build/fonts.bin build/fonts.inc &: $(FONTS_SRC) tools/gen_fonts.py
	@mkdir -p build
	python3 tools/gen_fonts.py build/fonts.bin build/fonts.inc

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
LINUX_CC ?= gcc
SLURP_SRC = slurp/linux.c slurp/reptyr/attach.c slurp/reptyr/reallocarray.c \
            slurp/reptyr/platform/linux/linux.c slurp/reptyr/platform/linux/linux_ptrace.c
SLURP_HEADERS = $(wildcard slurp/reptyr/*.h slurp/reptyr/platform/*.h slurp/reptyr/platform/linux/*.h slurp/reptyr/platform/linux/arch/*.h)

build/slurp-license.h: slurp/reptyr/COPYING
	@mkdir -p build
	python3 -c 'import json,pathlib; print("static const char slurp_license[] = "+json.dumps(pathlib.Path("slurp/reptyr/COPYING").read_text())+";")' > $@

build/slurp-linux: $(SLURP_SRC) $(SLURP_HEADERS) build/slurp-license.h
	@mkdir -p build
	$(LINUX_CC) -D_GNU_SOURCE -Os -Wall -Wextra -Wno-unused-parameter -static -s \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(SLURP_SRC)

build/slurp-version.h: build/slurp-linux
	python3 -c 'import hashlib,pathlib; p=pathlib.Path("build/slurp-linux"); print("#define SLURP_VERSION L\""+hashlib.sha256(p.read_bytes()).hexdigest()[:16]+"\"")' > $@
# which commit a build is (hardcore --version); the release number is HARDCORE_VERSION in hardcore.c
BUILD := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

hardcore.exe: hardcore.c hardcore_chrome.h hardcore_chrome_draw.h hardcore_display.h hardcore_reflow.h hardcore_structured_reflow.h hardcore_slurp.h slurp/reptyr/COPYING build/slurp-version.h build/font.bin build/fonts.bin build/fonts.inc build/width.inc build/res.o vendor/conpty/conpty.dll vendor/conpty/OpenConsole.exe
	$(CC) $(CFLAGS) -DFONT_BIN='"build/font.bin"' -DFONTS_BIN='"build/fonts.bin"' -DCONPTY_VER='L"$(CONPTY_VER)"' -DCONPTY_VER_A='"$(CONPTY_VER)"' \
		-DHARDCORE_BUILD='"$(BUILD)"' -o $@ hardcore.c build/res.o $(LIBS)

win-test: hardcore.exe reflow-test
	./hardcore.exe --unit "$$(wslpath -w build/unit.txt)"; rc=$$?; tail -1 build/unit.txt; [ $$rc = 0 ]
	python3 tests/run_xtermjs.py
	$(MAKE) --no-print-directory selftest
	$(MAKE) --no-print-directory slurp-test

slurp-test: hardcore.exe build/slurp-linux
	python3 tests/test_slurp_linux.py
	./hardcore.exe --slurp-selftest build/slurp-native-test.txt; rc=$$?; cat build/slurp-native-test.txt; [ $$rc = 0 ]
	./hardcore.exe --slurp-wsl-selftest build/slurp-wsl-test.txt; rc=$$?; cat build/slurp-wsl-test.txt; [ $$rc = 0 ]

bench/termbench.exe: bench/termbench.c
	$(CC) -O2 -Wall -o $@ $< -ld3d11 -ldxgi -lgdi32 -luser32 -luuid

selftest: hardcore.exe
	./hardcore.exe --selftest selftest.txt; rc=$$?; cat selftest.txt; exit $$rc

win-install: hardcore.exe
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/install.ps1 -Exe "$$(wslpath -w hardcore.exe 2>/dev/null || echo hardcore.exe)"

# ── macOS ──────────────────────────────────────────────────────────────

# The shader, compiled ahead of time when Xcode's metal compiler is there
# (the app compiles the same source at startup when it isn't).
HAVE_METAL := $(shell xcrun --find metal >/dev/null 2>&1 && echo 1)
ifeq ($(HAVE_METAL),1)
METALLIB = build/hardcore.metallib
MAC_FLAGS += -DHAVE_METALLIB
endif

build/hardcore.metallib: hardcore.metal
	@mkdir -p build
	xcrun metal -O2 -mmacosx-version-min=11.0 -c $< -o build/hardcore.air
	xcrun metallib build/hardcore.air -o $@

build/browser.inc: tools/embed_browser.py browser/project.js browser/welcome.html $(wildcard browser/adapters/*.js)
	python3 tools/embed_browser.py

build/hardcore: build/browser.inc hardcore_browser_mac.m hardcore_browser_label.h hardcore_browser_ui.h hardcore_browser_images.h hardcore_browser_pixels.h hardcore_browser_cli.h hardcore_browser_grid.h hardcore.c hardcore_chrome.h hardcore_chrome_draw.h hardcore_display.h hardcore_reflow.h hardcore_structured_reflow.h hardcore_mac.m hardcore_mac_update.h hardcore.metal build/font.bin build/fonts.bin build/fonts.inc build/width.inc $(METALLIB)
	$(MAC_CC) $(MAC_FLAGS) -x objective-c -DFONT_BIN='"build/font.bin"' -DFONTS_BIN='"build/fonts.bin"' -DHARDCORE_BUILD='"$(BUILD)"' -o $@ hardcore.c $(MAC_LIBS)

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

mac-test: build/hardcore reflow-test browser-test
	python3 tests/test_browser_cli.py
	build/hardcore --unit build/unit.txt; rc=$$?; tail -1 build/unit.txt; [ $$rc = 0 ]
	python3 tests/run_xtermjs.py
	build/hardcore --selftest build/selftest.txt; rc=$$?; cat build/selftest.txt; exit $$rc

clean:
	rm -rf build hardcore.exe selftest.txt $(APP)

# Upload this signed universal bundle as hardcore-macos.zip on a GitHub release.
.PHONY: release-mac
release-mac: $(APP)
	rm -f build/hardcore-macos.zip
	ditto -c -k --keepParent $(APP) build/hardcore-macos.zip

# Requires a logged-in desktop. Uses disposable app bundles and isolated config.
.PHONY: mac-update-test
mac-update-test: $(APP)
	python3 tests/test_update_mac.py

# Opt-in desktop integration: disposable real application sessions in Windows Terminal.
build/slurp-driver.exe: tests/slurp_driver.c hardcore.c hardcore_chrome.h hardcore_chrome_draw.h hardcore_display.h hardcore_reflow.h hardcore_structured_reflow.h hardcore_slurp.h slurp/reptyr/COPYING build/slurp-version.h build/font.bin build/fonts.bin build/fonts.inc build/width.inc build/res.o vendor/conpty/conpty.dll vendor/conpty/OpenConsole.exe
	$(CC) $(CFLAGS) -DFONT_BIN='"build/font.bin"' -DFONTS_BIN='"build/fonts.bin"' -DCONPTY_VER='L"$(CONPTY_VER)"' -DCONPTY_VER_A='"$(CONPTY_VER)"' \
		-o $@ tests/slurp_driver.c build/res.o $(filter-out -mwindows,$(LIBS))

.PHONY: slurp-app-test
slurp-app-test: build/slurp-driver.exe
	python3 tests/test_slurp_apps.py

# Pure display-only paragraph reflow: also runnable without a desktop.
.PHONY: reflow-test chrome-test
reflow-test:
	@mkdir -p build
	cc -std=c99 -O2 -Wall -Wextra tests/test_reflow.c -o build/test-reflow
	build/test-reflow

chrome-test: $(APP)
	python3 tests/test_chrome_mac.py

.PHONY: browser-test
browser-test: build/browser.inc
	cc -std=c99 -O2 -Wall -Wextra tests/test_browser_grid.c -o build/test-browser-grid
	build/test-browser-grid
	node --check browser/project.js
	node tests/test_browser_dom.js
	cc -std=c99 -O2 -Wall -Wextra -Wno-unused-function tests/browser_document_dump.c -o build/browser-document-dump
	node tests/test_wikipedia.js --preview
