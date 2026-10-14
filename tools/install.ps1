# Copies hardcore.exe to %LOCALAPPDATA%\hardcore and adds Start menu + desktop shortcuts.
# Pin it to the taskbar from the Start menu if you want it there.
param([Parameter(Mandatory = $true)][string]$Exe)
$dir = Join-Path $env:LOCALAPPDATA 'hardcore'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$dest = Join-Path $dir 'hardcore.exe'
# copies left by earlier installs, once nothing runs them any more
Get-ChildItem $dir -Filter 'hardcore.old-*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
# A running hardcore.exe can't be overwritten, but it can be renamed: it keeps
# running from the old copy, and the next window starts the new one.
if (Test-Path $dest) {
    try { [IO.File]::Open($dest, 'Open', 'ReadWrite', 'None').Close() }
    catch {
        $old = Join-Path $dir ("hardcore.old-{0}.exe" -f (Get-Date -Format 'yyyyMMddHHmmss'))
        Move-Item -Force $dest $old
        Write-Output "running copy moved aside: $old"
    }
}
Copy-Item -Force $Exe $dest
Write-Output "installed $dest"

foreach ($folder in 'Programs', 'Desktop') {
    $base = [Environment]::GetFolderPath($folder)
    $lnk = Join-Path $base 'hardcore.lnk'
    $sh = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
    $sh.TargetPath = $dest
    $sh.WorkingDirectory = $env:USERPROFILE
    $sh.IconLocation = "$dest,0"
    $sh.Description = 'A terminal for WSL'
    $sh.Save()
    Write-Output "shortcut  $lnk"
}
