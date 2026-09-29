# TSFix+ setup: installs or uninstalls TSFix+ in the Tales of Symphonia game folder.
# Run it through install.bat or uninstall.bat, from the game folder (the one with TOS.exe).
#
#   install    points Special K at tsfixplus.dll (d3d9.ini) and raises TSFix's frame limit to 1000
#              (tsfix.ini), after backing both files up
#   uninstall  undoes both edits and deletes TSFix+'s files
#
# Both .ini files are rewritten in the text encoding they were read in (Special K and TSFix save
# them as UTF-16), and nothing else in them changes.
param(
    [Parameter(Mandatory = $true)][ValidateSet('install', 'uninstall')][string]$Action,
    [string]$Folder = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Folder) { $Folder = $PSScriptRoot }   # the folder this script is in

function Fail([string]$message) {
    Write-Host ''
    Write-Host "Nothing was changed: $message" -ForegroundColor Red
    exit 1
}

function Read-Ini([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { $encoding = New-Object Text.UnicodeEncoding($false, $true) }
    elseif ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { $encoding = New-Object Text.UTF8Encoding($true) }
    else { $encoding = [Text.Encoding]::Default }
    return @{ Text = [IO.File]::ReadAllText($path, $encoding); Encoding = $encoding }
}

function Write-Ini([string]$path, $ini, [string]$text) {
    [IO.File]::WriteAllText($path, $text, $ini.Encoding)
}

# The text of one [section] of an .ini file, or $null.
function Get-Section([string]$text, [string]$name) {
    $m = [regex]::Match($text, '(?ms)^\[' + [regex]::Escape($name) + '\][^\[]*')
    if ($m.Success) { return $m } else { return $null }
}

$gameExe = Join-Path $Folder 'TOS.exe'
$d3d9Ini = Join-Path $Folder 'd3d9.ini'
$tsfixIni = Join-Path $Folder 'tsfix.ini'

if (-not (Test-Path $gameExe)) { Fail "this isn't the game folder (there's no TOS.exe here). Unzip TSFix+ into the Tales of Symphonia folder and run it from there." }
if (Get-Process -Name TOS -ErrorAction SilentlyContinue) { Fail 'the game is running. Close it and try again.' }
if (-not (Test-Path $d3d9Ini) -or -not (Test-Path $tsfixIni)) { Fail "TSFix doesn't seem to be installed (d3d9.ini or tsfix.ini is missing). Install TSFix first." }

$d3d9 = Read-Ini $d3d9Ini
$tsfix = Read-Ini $tsfixIni
$eol = if ($d3d9.Text.Contains("`r`n")) { "`r`n" } else { "`n" }

if ($Action -eq 'install') {
    if (-not (Test-Path (Join-Path $Folder 'tsfixplus.dll'))) { Fail 'tsfixplus.dll is missing. Unzip all of TSFix+ into the game folder first.' }

    # Backups, once: the first install keeps the files as they were before TSFix+.
    foreach ($f in @($d3d9Ini, $tsfixIni)) {
        $backup = "$f.before-tsfixplus"
        if (-not (Test-Path $backup)) { Copy-Item $f $backup }
    }

    # d3d9.ini: Special K loads TSFix+ as its Direct3D 9 "proxy".
    $text = $d3d9.Text
    $dgvoodoo = Get-Section $text 'Import.dgvoodoo'
    if ((Get-Section $text 'Import.TSFixPlus') -or ($dgvoodoo -and $dgvoodoo.Value -match '(?m)^Filename=tsfixplus\.dll')) {
        Write-Host 'd3d9.ini: already set up for TSFix+.'
    } elseif ($dgvoodoo) {
        # dgVoodoo is Special K's proxy: TSFix+ takes its place and loads dgVoodoo itself.
        # [^\r\n]* rather than .*: the line ending must stay as it is.
        $section = [regex]::Replace($dgvoodoo.Value, '(?m)^Filename=[^\r\n]*', 'Filename=tsfixplus.dll')
        $text = $text.Substring(0, $dgvoodoo.Index) + $section + $text.Substring($dgvoodoo.Index + $dgvoodoo.Length)
        Write-Host 'd3d9.ini: Special K now loads tsfixplus.dll (which loads dgVoodoo).'
    } else {
        $lines = @('[Import.TSFixPlus]', 'Architecture=Win32', 'Role=d3d9', 'When=Proxy', 'Filename=tsfixplus.dll', '')
        $text = ($lines -join $eol) + $eol + $text
        Write-Host 'd3d9.ini: added the [Import.TSFixPlus] section.'
    }
    Write-Ini $d3d9Ini $d3d9 $text

    # tsfix.ini: TSFix+ keeps the game at 30 itself; TSFix's limiter must stay out of its way.
    $text = [regex]::Replace($tsfix.Text, '(?m)^(ForegroundFPS|BackgroundFPS)=[0-9.]+', '$1=1000.0')
    Write-Ini $tsfixIni $tsfix $text
    Write-Host 'tsfix.ini: frame limit set to 1000 (TSFix+ keeps the game at 30 itself).'

    Write-Host ''
    Write-Host 'TSFix+ is installed. Start the game and press F9 to turn it on and off.' -ForegroundColor Green
    Write-Host "Your original d3d9.ini and tsfix.ini are saved as *.before-tsfixplus."
} else {
    # d3d9.ini: back to dgVoodoo, or remove the section install added.
    $text = $d3d9.Text
    $dgvoodoo = Get-Section $text 'Import.dgvoodoo'
    if ($dgvoodoo -and $dgvoodoo.Value -match '(?m)^Filename=tsfixplus\.dll') {
        $section = [regex]::Replace($dgvoodoo.Value, '(?m)^Filename=tsfixplus\.dll', 'Filename=dgVoodoo.dll')
        $text = $text.Substring(0, $dgvoodoo.Index) + $section + $text.Substring($dgvoodoo.Index + $dgvoodoo.Length)
    }
    $own = Get-Section $text 'Import.TSFixPlus'
    if ($own) { $text = $text.Substring(0, $own.Index) + $text.Substring($own.Index + $own.Length) }
    Write-Ini $d3d9Ini $d3d9 $text
    Write-Host 'd3d9.ini: Special K no longer loads TSFix+.'

    # tsfix.ini: back to TSFix's own 30 fps limit.
    $text = [regex]::Replace($tsfix.Text, '(?m)^(ForegroundFPS|BackgroundFPS)=[0-9.]+', '$1=30.0')
    Write-Ini $tsfixIni $tsfix $text
    Write-Host 'tsfix.ini: frame limit back to 30.'

    foreach ($f in @('tsfixplus.dll', 'tsfixplus.ini', 'tsfixplus.log')) {
        $p = Join-Path $Folder $f
        if (Test-Path $p) { Remove-Item $p }
    }
    Write-Host ''
    Write-Host 'TSFix+ is uninstalled. You can delete install.bat, uninstall.bat and tsfixplus-setup.ps1 too.' -ForegroundColor Green
}
