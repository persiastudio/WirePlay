<#
.SYNOPSIS
    Installs the libusb0 filter driver on the iPhone's USB composite device.

.DESCRIPTION
    The QuickTime capture backend talks to the phone through libusb. Apple's own
    driver owns the device, so libusb cannot see it -- which is why the engine
    reports "no iPhone found over USB" even with the cable plugged in and iTunes
    working perfectly.

    This installs libusb0.sys as an *UpperFilter* on the Apple usbccgp device
    instance. Apple's driver stays the function driver: iTunes, the Apple Devices
    app, 3uTools and backups all keep working, and libusb can reach the phone too.
    Both are usable at the same time.

    This is deliberately NOT the Zadig route. Zadig replaces Apple's driver with
    WinUSB, which breaks every one of those until it is undone. Do not use Zadig
    here.

    Run once, as administrator, with the iPhone plugged in and unlocked.
    Everything it changes is reverted by Uninstall-UsbFilter.ps1.

.NOTES
    Mechanism, package version and the iPhone hardware id are taken from
    0xbaksa/iphone-usb-reverse-tethering-windows (MIT), which does the same
    filter install for the same device on Windows 11.
#>

[CmdletBinding()]
param(
    # Where to unpack libusb-win32. Defaults to a folder next to this script.
    # Left empty here and resolved below: $PSScriptRoot is not populated while
    # the param block's defaults are being evaluated, so using it there fails
    # with "cannot bind an empty string to Path" when the script is run with
    # -File. Inside the body it is set.
    [string]$ToolsDir,

    # Skip the SHA-256 check on the downloaded archive. Off by default: this
    # script copies a kernel driver into System32, so the checksum matters.
    [switch]$SkipChecksum
)

$ErrorActionPreference = 'Stop'

# $scriptDir must exist in BOTH processes: the launching shell and the elevated
# child (which receives -ToolsDir already resolved, so any "resolve it while
# defaulting ToolsDir" trick leaves it undefined there -- that was a real
# failure: Join-Path $scriptDir on $null, red error, window gone).
$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }

if (-not $ToolsDir) {
    $ToolsDir = Join-Path $scriptDir '..\third_party\libusb-win32'
}

# Elevate if needed rather than failing with an access-denied wall of text.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList `
        "-NoProfile -NoExit -ExecutionPolicy Bypass -File `"$PSCommandPath`" -ToolsDir `"$ToolsDir`""
    exit
}

# Log everything to a file next to this script. The elevated window used to
# close the instant the script ended -- success or failure -- so a red error
# was gone before it could be read. The transcript survives the window:
# whoever ran this can paste tools\install_usb_filter.log instead.
$LogFile = Join-Path $scriptDir 'install_usb_filter.log'
try { Start-Transcript -Path $LogFile -Force | Out-Null } catch { }

$ZipUrl = 'https://github.com/mcuee/libusb-win32/releases/download/release_1.4.0.2/libusb-win32-bin-1.4.0.2.zip'
$ZipSha = '00004c92cdb99be36e17fb2377165eb97e63b48ba895bfc04a642ea9c3e26d94'
$Arch   = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }

function Step($n) { Write-Host "`n== $n" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "   $m" -ForegroundColor Green }
function Warn($m) { Write-Host "   $m" -ForegroundColor Yellow }

try {
    Step 'iPhone on USB'
    # PID varies by model, so it is read from the device rather than hardcoded.
    $phone = Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -match '^USB\\VID_05AC&PID_12A[0-9A-F]\\[^&]+$' } |
        Select-Object -First 1
    if (-not $phone) {
        throw 'No iPhone on USB. Plug it in with a data cable, unlock it, tap "Trust This Computer", then run this again.'
    }
    $id = $phone.InstanceId
    # "USB\VID_05AC&PID_12A8\<serial>" -> "USB\VID_05AC&PID_12A8"
    $hwid = ($id -split '\\')[0..1] -join '\'
    Ok "$($phone.FriendlyName) [$($phone.Status)]"
    Ok "$hwid"

    Step 'libusb-win32 1.4.0.2'
    $exe = Join-Path $ToolsDir "bin\$Arch\install-filter.exe"
    if (-not (Test-Path $exe)) {
        $tmp = Join-Path $env:TEMP 'libusb-win32-bin-1.4.0.2.zip'
        Invoke-WebRequest -Uri $ZipUrl -OutFile $tmp
        if (-not $SkipChecksum) {
            $h = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($h -ne $ZipSha) {
                Remove-Item $tmp -Force
                throw "SHA-256 mismatch for the libusb-win32 archive.`n  expected $ZipSha`n  got      $h"
            }
            Ok 'SHA-256 verified'
        }
        else { Warn 'checksum check skipped' }
        Expand-Archive -Path $tmp -DestinationPath $env:TEMP -Force
        # Move-Item will not create intermediate directories, and third_party\
        # does not exist on a fresh checkout -- which is what made this fail
        # with "part of the path could not be found" on a clean machine.
        $parent = Split-Path -Parent $ToolsDir
        if ($parent -and -not (Test-Path $parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        if (Test-Path $ToolsDir) { Remove-Item $ToolsDir -Recurse -Force }
        Move-Item (Join-Path $env:TEMP 'libusb-win32-bin-1.4.0.2') $ToolsDir -Force
    }

    # This is a kernel driver going into System32: refuse to load an unsigned one.
    $sig = Get-AuthenticodeSignature (Join-Path $ToolsDir 'bin\libusb0.cat')
    if ($sig.Status -ne 'Valid') { throw "libusb0.cat signature is not valid: $($sig.Status)" }
    Ok 'libusb0.cat signature valid'

    Step 'Filter driver on the Apple usbccgp device'
    $enumKey = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id"
    $already = (Get-Service libusb0 -ErrorAction SilentlyContinue) -and
               ((Get-ItemProperty $enumKey -ErrorAction SilentlyContinue).UpperFilters -contains 'libusb0')
    if ($already) {
        Ok 'already installed'
    }
    else {
        # "install-filter", not inf-wizard: this adds an UpperFilter alongside
        # Apple's driver instead of replacing it.
        & $exe install "--device=$hwid" | Out-Null
        # install-filter prints "err [set_device_state] calling class
        # installer failed" when it cannot restart the device, and this
        # script used to declare success anyway -- which is how a phone
        # ended up "installed" while libusb still could not see it. Trust
        # only the registry state of this device instance.
        $after = (Get-ItemProperty $enumKey -ErrorAction SilentlyContinue).UpperFilters
        if ($LASTEXITCODE -ne 0 -or -not ($after -contains 'libusb0')) {
            throw "install-filter did not stick on this device instance (exit $LASTEXITCODE). Unplug the iPhone, plug it back in, close anything using it (Apple Devices, 3uTools), and run this again."
        }
        Ok 'UpperFilter registered (verified in the registry)'
    }

    # The installer registers the service but does not always copy the driver
    # file, so put it there and start the service if it is not running.
    $sysDst = "$env:SystemRoot\System32\drivers\libusb0.sys"
    if (-not (Test-Path $sysDst) -or (Get-Service libusb0 -ErrorAction SilentlyContinue).Status -ne 'Running') {
        Copy-Item (Join-Path $ToolsDir "bin\$Arch\libusb0.sys") $sysDst -Force
        try { Start-Service libusb0 -ErrorAction Stop } catch { }
    }
    $svc = Get-Service libusb0 -ErrorAction SilentlyContinue
    Ok "libusb0 service: $(if ($svc) { $svc.Status } else { 'not registered' })"

    # pyusb loads libusb0.dll from the working directory or PATH.
    Copy-Item (Join-Path $ToolsDir "bin\$Arch\libusb0.dll") (Join-Path $scriptDir 'libusb0.dll') -Force
    Ok 'libusb0.dll placed in tools\'

    Step 'Verification'
    Start-Sleep 2
    & python -c "import usb.core; d=list(usb.core.find(find_all=True)); print('   libusb sees', len(d), 'device(s)'); [print('    ', hex(x.idVendor)+':'+hex(x.idProduct)) for x in d]"
    if ($LASTEXITCODE -ne 0) {
        Warn 'pyusb could not enumerate. Copy tools\libusb0.dll next to the engine, or run wireplay from the tools\ folder.'
    }

    Step 'Done'
    Ok 'Apple''s driver is untouched -- iTunes and 3uTools still work.'
    Write-Host "`nNow run:" -ForegroundColor Cyan
    Write-Host '  .\build\Release\wireplay.exe --backend quicktime --engine "python engine\wireplay_engine.py" -v'
}
catch {
    Write-Host "`nFAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Full log: $LogFile" -ForegroundColor Yellow
    try { Stop-Transcript | Out-Null } catch { }
    # Hold the (possibly elevated) window open so the error can be read.
    try { Read-Host 'Press Enter to close' } catch { }
    exit 1
}
try { Stop-Transcript | Out-Null } catch { }
try { Read-Host 'Done. Press Enter to close' } catch { }
