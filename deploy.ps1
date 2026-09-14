# Deploy freshly built binaries to C:\Tools\pathscope. Run elevated.
# A loaded DLL cannot be overwritten, but it CAN be renamed - so rename the old
# one out of the way (unique name, so a stale .old never blocks us) and copy in
# the new one. Verifies by hash and fails loudly.
$ErrorActionPreference = 'Stop'
$log = "$env:TEMP\pathscope-deploy.log"
Start-Transcript -Path $log -Force | Out-Null

# NOTE: PowerShell variables are case-insensitive - do not name a loop variable
# $src while using $SRC as the base path; the loop silently clobbers the base.
$D    = 'C:\Tools\pathscope'
$ROOT = 'E:\cpp\pathscope'
$ok   = $true

try {
    # best-effort cleanup of previous .old files (ignore ones still loaded)
    Get-ChildItem "$D\*.old*" -ErrorAction SilentlyContinue | ForEach-Object {
        try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch {}
    }

    foreach ($a in 'x64','x86') {
        foreach ($f in "pathscope_$a.dll", "pshostw_$a.exe", "pshost_$a.exe") {
            $srcFile = Join-Path "$ROOT\build\$a\Release" $f
            $dst = Join-Path $D $f
            if (-not (Test-Path $srcFile)) { Write-Output "MISSING BUILD: $srcFile"; $ok = $false; continue }

            $want = (Get-FileHash $srcFile).Hash

            if (Test-Path $dst) {
                if ((Get-FileHash $dst).Hash -eq $want) { Write-Output "unchanged  $f"; continue }
                try {
                    Copy-Item $srcFile $dst -Force -ErrorAction Stop
                } catch {
                    $bak = "$dst.old.$(Get-Random)"
                    Rename-Item $dst $bak -Force -ErrorAction Stop
                    Copy-Item $srcFile $dst -Force -ErrorAction Stop
                }
            } else {
                Copy-Item $srcFile $dst -Force -ErrorAction Stop
            }

            $got = (Get-FileHash $dst).Hash
            if ($got -eq $want) { Write-Output "DEPLOYED   $f  $($got.Substring(0,12))" }
            else { Write-Output "HASH MISMATCH $f  want=$($want.Substring(0,12)) got=$($got.Substring(0,12))"; $ok = $false }
        }
    }

    foreach ($f in 'CMakeLists.txt','pathscope.c','pshost.c','pshostw.c','pathscope.def','README.md') {
        $s = Join-Path $ROOT $f
        if (Test-Path $s) { Copy-Item $s (Join-Path $D $f) -Force }
    }

    Write-Output $(if ($ok) { 'DEPLOY-OK' } else { 'DEPLOY-FAILED' })
}
catch {
    Write-Output "DEPLOY-FAILED: $($_.Exception.Message)"
}
finally {
    Stop-Transcript | Out-Null
}

