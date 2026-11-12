# Probe: does upstream libjuice `origin/turn-tcp` compile?
# Scratch build lives under build/ so the project root stays clean.
$ErrorActionPreference = "Stop"
$repo = "D:\MyOpen\NimRTC\src\third_party\libjuice\src"
$out  = "D:\MyOpen\NimRTC\build\turntcp_probe"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $repo
git archive origin/turn-tcp | tar -x -C $out
Pop-Location
if (-not (Test-Path "$out\CMakeLists.txt")) { Write-Output "PROBE_FAIL: archive did not produce CMakeLists.txt"; exit 1 }
Write-Output "PROBE: extracted OK"
Push-Location $out
cmake -S . -B b -DBUILD_SHARED_LIBS=OFF -DNO_SERVER=ON -DNO_TESTS=ON -DCMAKE_BUILD_TYPE=Release 2>&1 | Select-Object -Last 5
cmake --build b 2>&1 | Select-String -Pattern "error|Error" | Select-Object -First 40
Write-Output "PROBE_EXIT=$LASTEXITCODE"
Pop-Location
