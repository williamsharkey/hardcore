# Starts hardcore as administrator at log on, without a UAC prompt each time:
# a scheduled task "hardcore-elevated" (At log on, your user only, Run with
# highest privileges). Run it once from an elevated PowerShell:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win-elevated-login.ps1
# -Execute/-Arguments: what the task starts (default: hardcore, as the Start menu
# starts it). -Remove: delete the task. See README "Run as administrator".
param(
    [string]$User = "$env:USERDOMAIN\$env:USERNAME",
    [string]$Execute = (Join-Path $env:LOCALAPPDATA 'hardcore\hardcore.exe'),
    [string]$Arguments = '--detached',
    [string]$TaskName = 'hardcore-elevated',
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole('Administrators')
if (-not $admin) { throw 'run this from an elevated PowerShell (Run as administrator)' }
if ($Remove) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Output "removed task $TaskName"
    return
}
$action = New-ScheduledTaskAction -Execute $Execute -Argument $Arguments -WorkingDirectory $env:USERPROFILE
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $User
$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Highest
# Priority 4: a task's default (7) runs it, hardcore and every tab below normal priority.
# No time limit; a laptop on battery starts it too.
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -Priority 4
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings `
    -Description 'hardcore as administrator at log on (README: Run as administrator)' -Force | Out-Null
$t = Get-ScheduledTask -TaskName $TaskName
Write-Output ("task {0}: {1} {2}; user {3}; run level {4}" -f $TaskName, $t.Actions[0].Execute, $t.Actions[0].Arguments, $t.Principal.UserId, $t.Principal.RunLevel)
