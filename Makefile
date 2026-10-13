# wsl-easy — build from WSL (mingw-w64 cross compiler) or MSYS2.
#
#   make             → wsl-easy.exe
#   make selftest    → headless ConPTY/WSL/parser check (run from WSL or Windows)
#   make install     → copy to %LOCALAPPDATA%\wsl-easy\ and add Start menu + desktop shortcuts

CC      ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LIBS     = -mwindows -municode -lopengl32 -lgdi32 -luser32 -lkernel32 -lshell32 -static

ifeq ($(origin CC),default)
CC = x86_64-w64-mingw32-gcc
endif

.PHONY: all clean selftest install

all: wsl-easy.exe

build/font.bin: font/unifont-13.0.06.hex tools/gen_font.py
	@mkdir -p build
	python3 tools/gen_font.py $< $@

build/wsl-easy.ico: tools/gen_icon.py
	@mkdir -p build
	python3 tools/gen_icon.py $@

build/res.o: wsl-easy.rc build/wsl-easy.ico
	$(WINDRES) $< -O coff -o $@

wsl-easy.exe: wsl-easy.c build/font.bin build/res.o
	$(CC) $(CFLAGS) -DFONT_BIN='"build/font.bin"' -o $@ wsl-easy.c build/res.o $(LIBS)

selftest: wsl-easy.exe
	./wsl-easy.exe --selftest selftest.txt; rc=$$?; cat selftest.txt; exit $$rc

install: wsl-easy.exe
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/install.ps1 -Exe "$$(wslpath -w wsl-easy.exe 2>/dev/null || echo wsl-easy.exe)"

clean:
	rm -rf build wsl-easy.exe selftest.txt
