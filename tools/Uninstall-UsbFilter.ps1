<#
.SYNOPSIS
    Removes the libusb0 UpperFilter that install_usb_filter.ps1 added.
.DESCRIPTION
    Rolls the iPhone's device key back to Apple's driver alone. Run as
    administrator with the phone plugged in.
#>
[CmdletBinding()]
# $ToolsDir is resolved in the body, not as a param default: $PSScriptRoot is
# empty while param defaults are evaluated, which fails under -File.
param([string]$ToolsDir)

$ErrorActionPreference = 'Stop'

if (-not $ToolsDir) {
    $here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
    $ToolsDir = Join-Path $here '..\third_party\libusb-win32'
}
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList `
        "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -ToolsDir `"$ToolsDir`""
    exit
}

$Arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }
$exe  = Join-Path $ToolsDir "bin\$Arch\install-filter.exe"
if (-not (Test-Path $exe)) { throw "libusb-win32 not found at $ToolsDir; nothing to uninstall with." }

& $exe uninstall --all-devices
Write-Host "Filter removed. Apple's driver is now the only one on the device." -ForegroundColor Green
Write-Host "System32\drivers\libusb0.sys is left in place; it is inert without the filter."
