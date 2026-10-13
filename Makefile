# wsl-easy — build from WSL (mingw-w64 cross compiler) or MSYS2.
#
#   make             → wsl-easy.exe
#   make test        → unit tests + xterm.js fixtures + headless ConPTY/WSL selftest
#   make bench/termbench.exe → the measuring tool (see bench/RESULTS.md)
#   make install     → copy to %LOCALAPPDATA%\wsl-easy\ and add Start menu + desktop shortcuts

CC      ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LIBS     = -mwindows -municode -lopengl32 -lgdi32 -luser32 -lkernel32 -lshell32 -static

ifeq ($(origin CC),default)
CC = x86_64-w64-mingw32-gcc
endif

.PHONY: all clean selftest install test

all: wsl-easy.exe

build/font.bin: font/unifont-13.0.06.hex tools/gen_font.py
	@mkdir -p build
	python3 tools/gen_font.py $< $@

build/wsl-easy.ico: tools/gen_icon.py
	@mkdir -p build
	python3 tools/gen_icon.py $@

build/res.o: wsl-easy.rc build/wsl-easy.ico
	$(WINDRES) $< -O coff -o $@

CONPTY_VER = 1.25.260930003

wsl-easy.exe: wsl-easy.c build/font.bin build/res.o vendor/conpty/conpty.dll vendor/conpty/OpenConsole.exe
	$(CC) $(CFLAGS) -DFONT_BIN='"build/font.bin"' -DCONPTY_VER='L"$(CONPTY_VER)"' -DCONPTY_VER_A='"$(CONPTY_VER)"' \
		-o $@ wsl-easy.c build/res.o $(LIBS)

test: wsl-easy.exe
	./wsl-easy.exe --unit "$$(wslpath -w build/unit.txt)"; rc=$$?; tail -1 build/unit.txt; [ $$rc = 0 ]
	python3 tests/run_xtermjs.py
	$(MAKE) --no-print-directory selftest

bench/termbench.exe: bench/termbench.c
	$(CC) -O2 -Wall -o $@ $< -ld3d11 -ldxgi -lgdi32 -luser32 -luuid

selftest: wsl-easy.exe
	./wsl-easy.exe --selftest selftest.txt; rc=$$?; cat selftest.txt; exit $$rc

install: wsl-easy.exe
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/install.ps1 -Exe "$$(wslpath -w wsl-easy.exe 2>/dev/null || echo wsl-easy.exe)"

clean:
	rm -rf build wsl-easy.exe selftest.txt
