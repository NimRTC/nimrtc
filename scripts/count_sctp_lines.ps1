$ErrorActionPreference = 'Stop'
$root = 'D:\MyOpen\NimRTC'
$files = Get-ChildItem -Recurse -Path (Join-Path $root 'src\sctp') -Include *.hpp,*.cpp,'CMakeLists.txt'
foreach ($f in $files) {
    $rel = $f.FullName.Substring($root.Length + 1)
    $lines = (Get-Content -LiteralPath $f.FullName | Measure-Object -Line).Lines
    $fmt = '{0,-6} {1}'
    $out = $fmt -f $lines, $rel
    Write-Output $out
}
$root2 = $root
$rootfile = Join-Path $root 'CMakeLists.txt'
$diff = (Get-Content -LiteralPath $rootfile | Measure-Object -Line).Lines
Write-Output ('{0,-6} {1}' -f $diff, 'CMakeLists.txt (root, modified +1 line)')
