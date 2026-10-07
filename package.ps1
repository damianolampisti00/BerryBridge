# Builds Berry Bridge (UI app + headless service) and packages ONE .bar,
# bumping <buildId> in BerryBridgeUI/bar-descriptor.xml first so BB10 accepts
# it as an in-place update (version must increase).
#
# Usage:  powershell -File package.ps1
#         powershell -File package.ps1 -Install               (also copies it to
#                     the phone and installs it, over SSH as root)
#         powershell -File package.ps1 -Install -PhoneIp 192.168.1.xxx -RootKey C:\path\to\id_rsa
#
# -PhoneIp / -RootKey default to $PhoneIp / $RootKey set in package.config.ps1
# next to this script (not in git; copy package.config.example.ps1).
#
# Needs the BlackBerry 10 Native SDK 10.3 in C:\bbndk (Momentics' default) and
# Git for Windows. The NDK's own JRE 1.7 runs the packager (a newer system Java
# crashes it); the .bar is packaged with -devMode and installed unsigned by
# tools/installbar.sh (QNX's sud_install_package), which needs a ROOTED phone
# reachable over SSH as root.
param(
    [switch]$Install,
    [string]$PhoneIp,
    [string]$RootKey
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$config = Join-Path $root 'package.config.ps1'
if (Test-Path $config) {
    $cliPhoneIp = $PhoneIp; $cliRootKey = $RootKey
    . $config
    if ($cliPhoneIp) { $PhoneIp = $cliPhoneIp }
    if ($cliRootKey) { $RootKey = $cliRootKey }
}
$ui = "$root\BerryBridgeUI"
$svc = "$root\BerryBridgeUIService"
$h = 'C:\bbndk\ndk\host_10_3_1_12\win32\x86'
$jre = 'C:\bbndk\features\com.qnx.tools.jre.win32.x86_64_1.7.0.51\jre'  # system Java 25 crashes the packager
$env:QNX_HOST = $h
$env:QNX_TARGET = 'C:\bbndk\ndk\target_10_3_1_995\qnx6'
$env:JAVA_HOME = $jre
$env:PATH = "$jre\bin;$h\usr\bin;$env:PATH"

# --- bump build id ---
$descPath = "$ui\bar-descriptor.xml"
$desc = Get-Content $descPath -Raw
$build = [int]([regex]::Match($desc, '<buildId>(\d+)</buildId>').Groups[1].Value) + 1
$desc = $desc -replace '<buildId>\d+</buildId>', "<buildId>$build</buildId>"
$ver = [regex]::Match($desc, '<versionNumber>([^<]+)</versionNumber>').Groups[1].Value
Set-Content $descPath $desc -Encoding UTF8 -NoNewline
$full = "$ver.$build"

# --- build both binaries (out of tree; delete build\<x>\Makefile after a .pro change) ---
function Build-Project([string]$pro, [string]$dir) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    Push-Location $dir
    try {
        if (-not (Test-Path Makefile)) {
            qmake $pro -spec blackberry-armv7le-qcc CONFIG+=device CONFIG+=release
            if ($LASTEXITCODE -ne 0) { throw "qmake failed for $pro" }
        }
        make -j4
        if ($LASTEXITCODE -ne 0) { throw "make failed for $pro" }
    } finally { Pop-Location }
}
Build-Project "$ui\BerryBridgeUI.pro" "$root\build\ui"
Build-Project "$svc\BerryBridgeUIService.pro" "$root\build\service"

# --- stage + package ---
$s = "$root\stage"
Remove-Item $s -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$s\arm\o.le-v7", "$s\translations" | Out-Null
Copy-Item "$root\build\ui\o.le-v7\BerryBridgeUI.so" "$s\arm\o.le-v7\"
Copy-Item "$root\build\service\BerryBridgeUIService" "$s\arm\o.le-v7\"   # a plain app target: no o.le-v7 subdir
Copy-Item "$ui\assets" "$s\assets" -Recurse
Copy-Item "$ui\translations\*.qm" "$s\translations\"
Copy-Item "$ui\icon.png", $descPath $s
Push-Location $s
$bar = "$root\BerryBridge-$full-armv7.bar"
blackberry-nativepackager -package $bar bar-descriptor.xml -configuration Device-Release -devMode
$packOk = ($LASTEXITCODE -eq 0)
Pop-Location
if (-not $packOk) { throw 'packaging failed' }
Write-Host "Created $bar (version $full)"

if (-not $Install) { exit 0 }
if (-not $PhoneIp -or -not $RootKey) { throw '-Install needs -PhoneIp and -RootKey (or package.config.ps1)' }

# --- copy to the phone and install, over SSH as root, through Git Bash: not
# PowerShell's own ssh/scp, whose scp reads "C:\..." (a colon before the first
# slash) as a remote "host:path" ---
$barName = Split-Path $bar -Leaf
$installer = "$root\tools\installbar.sh"
$toPosix = { param($w) '/' + $w.Substring(0, 1).ToLower() + $w.Substring(2).Replace('\', '/') }
$barPosix = & $toPosix $bar
$installerPosix = & $toPosix $installer
$keyPosix = & $toPosix $RootKey
$bashScript = @"
set -e
SSH=(ssh -i "$keyPosix" -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa -o KexAlgorithms=diffie-hellman-group14-sha1 -o Ciphers=aes128-cbc -o MACs=hmac-sha1 -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@$PhoneIp)
echo "Copying $barName and installbar.sh to the phone ($PhoneIp)..."
`"`${SSH[@]}`" "cat > /accounts/1000/shared/downloads/$barName" < "$barPosix"
`"`${SSH[@]}`" "cat > /accounts/1000/shared/downloads/installbar.sh" < "$installerPosix"
echo "Installing..."
`"`${SSH[@]}`" "cat /accounts/1000/shared/downloads/installbar.sh | sh -s -- $barName"
"@
$scriptFile = Join-Path $env:TEMP 'bb_install.sh'
[System.IO.File]::WriteAllText($scriptFile, $bashScript.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
& 'C:\Program Files\Git\bin\bash.exe' (& $toPosix $scriptFile)
if ($LASTEXITCODE -ne 0) { throw "install over SSH failed (exit $LASTEXITCODE) -- is the phone reachable at $PhoneIp?" }
Remove-Item $scriptFile -ErrorAction SilentlyContinue
Write-Host "Installed $barName on the phone."
