# ConPTY (OpenConsole)

`conpty.dll` and `OpenConsole.exe` (x64) from the NuGet package
[Microsoft.Windows.Console.ConPTY 1.25.260930003](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.25.260930003),
built from [microsoft/terminal](https://github.com/microsoft/terminal). MIT License,
Copyright (c) Microsoft Corporation.

This is the same pseudoconsole that Windows Terminal ships. The ConPTY inside
Windows (conhost.exe) re-renders every frame itself, and in our measurements it
spent about a full core doing so. hardcore embeds these two files, extracts them
to `%LOCALAPPDATA%\hardcore\conpty-1.25.260930003\` on first run, and falls back
to the inbox ConPTY if that fails (or with `--inbox-conpty`).

To update: download the .nupkg, take `runtimes/win-x64/native/conpty.dll` and
`build/native/runtimes/x64/OpenConsole.exe`, and bump `CONPTY_VER` in the Makefile.
