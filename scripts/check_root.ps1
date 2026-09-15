$ErrorActionPreference = 'Stop'
$root = 'D:\MyOpen\NimRTC'
Get-ChildItem -Path $root -File | ForEach-Object {
    $name = $_.Name
    if ($name -match '\.(ps1|py|sh|bat|cmd|tar\.gz|zip)$') {
        Write-Output ("FORBIDDEN: {0}" -f $name)
    }
}
