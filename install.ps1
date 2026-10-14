# hardcore installer. In PowerShell:
#   irm https://raw.githubusercontent.com/williamsharkey/hardcore/main/install.ps1 | iex
# Downloads the latest release's hardcore.exe, checks it against the SHA-256
# GitHub publishes for it, puts it in %LOCALAPPDATA%\hardcore, adds Start menu
# and desktop shortcuts, and starts it. Run it again to update (or, from a
# hardcore tab, `hardcore update`). One exe, nothing else installed.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'     # Invoke-WebRequest's progress bar makes it many times slower
$repo = 'williamsharkey/hardcore'
$dir = if ($env:HARDCORE_INSTALL_DIR) { $env:HARDCORE_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'hardcore' }
$quiet = [bool]$env:HARDCORE_INSTALL_QUIET   # tests: no shortcuts, don't start it

$rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'hardcore-install' }
$asset = $rel.assets | Where-Object { $_.name -eq 'hardcore.exe' } | Select-Object -First 1
if (-not $asset) { throw "hardcore: release $($rel.tag_name) has no hardcore.exe" }
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$dest = Join-Path $dir 'hardcore.exe'
$tmp = "$dest.download"
Write-Host "hardcore $($rel.tag_name): downloading..."
Invoke-WebRequest $asset.browser_download_url -OutFile $tmp -UseBasicParsing
if ($asset.digest -like 'sha256:*') {
    $got = (Get-FileHash $tmp -Algorithm SHA256).Hash
    if ($got -ne $asset.digest.Substring(7)) { Remove-Item $tmp; throw "hardcore: the download doesn't match GitHub's SHA-256" }
    Write-Host 'hardcore: SHA-256 checks out'
}
# a running hardcore.exe can't be overwritten, but it can be renamed
Get-ChildItem $dir -Filter 'hardcore.old-*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
if (Test-Path $dest) {
    try { [IO.File]::Open($dest, 'Open', 'ReadWrite', 'None').Close() }
    catch { Move-Item -Force $dest (Join-Path $dir ('hardcore.old-{0}.exe' -f (Get-Date -Format 'yyyyMMddHHmmss'))) }
}
Move-Item -Force $tmp $dest
Write-Host "hardcore: installed $dest"
if (-not $quiet) {
    foreach ($folder in 'Programs', 'Desktop') {
        $lnk = Join-Path ([Environment]::GetFolderPath($folder)) 'hardcore.lnk'
        $sh = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
        $sh.TargetPath = $dest
        $sh.WorkingDirectory = $env:USERPROFILE
        $sh.IconLocation = "$dest,0"
        $sh.Description = 'A terminal for WSL'
        $sh.Save()
    }
    Write-Host 'hardcore: Start menu and desktop shortcuts added'
    Start-Process $dest
}
