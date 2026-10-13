# Copies hardcore.exe to %LOCALAPPDATA%\hardcore and adds Start menu + desktop shortcuts.
# Pin it to the taskbar from the Start menu if you want it there.
param([Parameter(Mandatory = $true)][string]$Exe)
$dir = Join-Path $env:LOCALAPPDATA 'hardcore'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$dest = Join-Path $dir 'hardcore.exe'
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
