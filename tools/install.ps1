# Copies hardcore.exe to %LOCALAPPDATA%\hardcore and adds Start menu + desktop shortcuts.
# Pin it to the taskbar from the Start menu if you want it there.
param([Parameter(Mandatory = $true)][string]$Exe)
$dir = Join-Path $env:LOCALAPPDATA 'hardcore'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$dest = Join-Path $dir 'hardcore.exe'
Copy-Item -Force $Exe $dest
Write-Output "installed $dest"

# This used to be called wsl-easy: keep its settings, drop its shortcuts and files.
$old = Join-Path $env:LOCALAPPDATA 'wsl-easy'
if (Test-Path $old) {
    $cfg = Join-Path $old 'config.ini'
    if ((Test-Path $cfg) -and -not (Test-Path (Join-Path $dir 'config.ini'))) { Copy-Item $cfg $dir }
    Remove-Item -Recurse -Force $old -ErrorAction SilentlyContinue
    Write-Output "migrated  $old"
}

foreach ($folder in 'Programs', 'Desktop') {
    $base = [Environment]::GetFolderPath($folder)
    $was = Join-Path $base 'wsl-easy.lnk'
    if (Test-Path $was) { Remove-Item -Force $was; Write-Output "removed   $was" }
    $lnk = Join-Path $base 'hardcore.lnk'
    $sh = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
    $sh.TargetPath = $dest
    $sh.WorkingDirectory = $env:USERPROFILE
    $sh.IconLocation = "$dest,0"
    $sh.Description = 'A terminal for WSL'
    $sh.Save()
    Write-Output "shortcut  $lnk"
}
