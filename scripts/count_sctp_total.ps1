$ErrorActionPreference = 'Stop'
$root = 'D:\MyOpen\NimRTC'
$total = 0
$files = Get-ChildItem -Recurse -Path (Join-Path $root 'src\sctp') -Include *.hpp,*.cpp,'CMakeLists.txt'
foreach ($f in $files) {
    $lines = (Get-Content -LiteralPath $f.FullName | Measure-Object -Line).Lines
    $total += $lines
}
Write-Output "Total sctp LOC (hpp+cpp+CMakeLists.txt): $total"
