# Copies wsl-easy.exe to %LOCALAPPDATA%\wsl-easy and adds a Start menu shortcut.
# Pin it to the taskbar from the Start menu if you want it there.
param([Parameter(Mandatory = $true)][string]$Exe)
$dir = Join-Path $env:LOCALAPPDATA 'wsl-easy'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$dest = Join-Path $dir 'wsl-easy.exe'
Copy-Item -Force $Exe $dest
$lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'wsl-easy.lnk'
$sh = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
$sh.TargetPath = $dest
$sh.WorkingDirectory = $env:USERPROFILE
$sh.Description = 'Tabbed terminal for WSL'
$sh.Save()
Write-Output "installed $dest"
Write-Output "shortcut  $lnk"
