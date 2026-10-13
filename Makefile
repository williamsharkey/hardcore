# hardcore — build from WSL (mingw-w64 cross compiler) or MSYS2.
#
#   make             → hardcore.exe
#   make test        → unit tests + xterm.js fixtures + headless ConPTY/WSL selftest
#   make bench/termbench.exe → the measuring tool (see bench/RESULTS.md)
#   make install     → copy to %LOCALAPPDATA%\hardcore\ and add Start menu + desktop shortcuts

CC      ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LIBS     = -mwindows -municode -ld3d11 -ldxgi -luuid -lgdi32 -luser32 -lkernel32 -lshell32 -static

ifeq ($(origin CC),default)
CC = x86_64-w64-mingw32-gcc
endif

.PHONY: all clean selftest install test

all: hardcore.exe

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

test: hardcore.exe
	./hardcore.exe --unit "$$(wslpath -w build/unit.txt)"; rc=$$?; tail -1 build/unit.txt; [ $$rc = 0 ]
	python3 tests/run_xtermjs.py
	$(MAKE) --no-print-directory selftest

bench/termbench.exe: bench/termbench.c
	$(CC) -O2 -Wall -o $@ $< -ld3d11 -ldxgi -lgdi32 -luser32 -luuid

selftest: hardcore.exe
	./hardcore.exe --selftest selftest.txt; rc=$$?; cat selftest.txt; exit $$rc

install: hardcore.exe
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/install.ps1 -Exe "$$(wslpath -w hardcore.exe 2>/dev/null || echo hardcore.exe)"

clean:
	rm -rf build hardcore.exe selftest.txt
