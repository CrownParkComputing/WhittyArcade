# MANX test-machine setup for Windows. Run once, in an ADMINISTRATOR
# PowerShell, on a machine you want to test MANX multiplayer on:
#
#     powershell -ExecutionPolicy Bypass -File tools\setup-machine.ps1
#
# It opens remote access so builds can be pushed and logs read back, and
# reports what the machine looks like from the network.
#
# Everything here is idempotent: a second run changes nothing.

$ErrorActionPreference = 'Continue'

# The public half of the key pair on the development machine. Only ever
# appended to an authorized_keys file; the private half never leaves the
# machine that made it.
$PubKey = 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGj8ZcfjenWNyfKybRCyv1uR/v4Mnem5UkEOJc20Fbv8 manx-push-from-cachyos'

function Say  ($m) { Write-Host "`n== $m" -ForegroundColor White }
function Ok   ($m) { Write-Host "   ok   $m" -ForegroundColor Green }
function Note ($m) { Write-Host "   note $m" -ForegroundColor Yellow }
function Bad  ($m) { Write-Host "   fail $m" -ForegroundColor Red }

$admin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host "Run this from an Administrator PowerShell: installing the SSH"
    Write-Host "server and adding firewall rules both need it."
    exit 1
}

Say "Machine"
Write-Host "   user : $env:USERNAME"
Write-Host "   host : $env:COMPUTERNAME"
Write-Host "   os   : $((Get-CimInstance Win32_OperatingSystem).Caption)"

# ------------------------------------------------------------- ssh server
Say "SSH server"
$cap = Get-WindowsCapability -Online -Name 'OpenSSH.Server*' -ErrorAction SilentlyContinue
if ($cap -and $cap.State -ne 'Installed') {
    Note "installing the OpenSSH server feature (this takes a minute)"
    Add-WindowsCapability -Online -Name $cap.Name | Out-Null
}
if (Get-Service sshd -ErrorAction SilentlyContinue) {
    Set-Service -Name sshd -StartupType Automatic
    Start-Service sshd -ErrorAction SilentlyContinue
    if ((Get-Service sshd).Status -eq 'Running') { Ok "sshd running" }
    else { Bad "sshd would not start - check: Get-Service sshd" }
} else {
    Bad "no sshd service - install OpenSSH Server from Optional Features"
}

# --------------------------------------------------------------- firewall
# A connection that times out rather than being refused means a firewall is
# dropping it, which is what Windows does by default.
Say "Firewall"
if (-not (Get-NetFirewallRule -DisplayName 'MANX remote access' -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName 'MANX remote access' -Direction Inbound `
        -Protocol TCP -LocalPort 22 -Action Allow -Profile Any | Out-Null
    Ok "allowed inbound TCP 22"
} else {
    Ok "TCP 22 already allowed"
}

# MANX itself needs no port opening - discovery works through the firewall's
# own connection tracking. What Windows does do is prompt the first time the
# program listens, and a prompt nobody answers looks exactly like a network
# that does not work. Pre-authorising the binary skips it.
$here = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe  = Join-Path $here 'build-clean\MANX.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $here 'MANX.exe' }
if (Test-Path $exe) {
    if (-not (Get-NetFirewallRule -DisplayName 'MANX' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName 'MANX' -Direction Inbound `
            -Program $exe -Action Allow -Profile Private,Domain | Out-Null
        Ok "pre-authorised MANX so Windows will not prompt"
    } else {
        Ok "MANX already authorised"
    }
} else {
    Note "no MANX.exe found here yet - Windows will prompt on first run;"
    Note "answer yes, and tick Private networks"
}

# -------------------------------------------------------------------- key
Say "Remote access key"
# Windows OpenSSH reads administrators_authorized_keys for any account in the
# Administrators group, and ignores the per-user file for them.
$isAdminUser = (Get-LocalGroupMember -Group 'Administrators' -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like "*\$env:USERNAME" }) -ne $null
if ($isAdminUser) {
    $keyFile = "$env:ProgramData\ssh\administrators_authorized_keys"
} else {
    $keyFile = "$env:USERPROFILE\.ssh\authorized_keys"
}
New-Item -ItemType Directory -Force -Path (Split-Path $keyFile) | Out-Null
if (-not (Test-Path $keyFile)) { New-Item -ItemType File -Path $keyFile | Out-Null }
$fingerprint = ($PubKey -split ' ')[1]
if (Select-String -Path $keyFile -SimpleMatch $fingerprint -Quiet) {
    Ok "key already present"
} else {
    Add-Content -Path $keyFile -Value $PubKey
    Ok "key added to $keyFile"
}
if ($isAdminUser) {
    # That file must be readable only by SYSTEM and Administrators or sshd
    # refuses to use it, silently, and the login just fails.
    icacls $keyFile /inheritance:r /grant 'Administrators:F' /grant 'SYSTEM:F' | Out-Null
    Ok "permissions tightened (sshd ignores this file otherwise)"
}

# ----------------------------------------------------------------- report
Say "Reachable at"
$addrs = Get-NetIPAddress -AddressFamily IPv4 |
         Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' }
if (-not $addrs) {
    Bad "no IPv4 address - is this machine on the network?"
} else {
    foreach ($a in $addrs) { Write-Host "   $env:USERNAME@$($a.IPAddress)" }
}

Say "Done"
Write-Host "   Report the user@address line above and builds can be pushed and"
Write-Host "   this machine's logs read directly."
