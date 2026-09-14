$session = New-Object -ComObject Microsoft.Update.Session
$searcher = $session.CreateUpdateSearcher()
try {
    $updates = $searcher.Search('IsInstalled=0 and Type=''Software''').Updates
    $driverUpdates = $updates | Where-Object { $_.Title -match 'NVIDIA' -or $_.Title -match 'GeForce' -or $_.Title -match 'display' }
    if ($driverUpdates) {
        $driverUpdates | ForEach-Object { Write-Host $_.Title }
    } else {
        Write-Host 'No NVIDIA/display driver updates found via Windows Update'
    }
} catch {
    Write-Host "Error searching Windows Update: $($_.Exception.Message)"
}

Write-Host '---'

try {
    $searcher2 = $session.CreateUpdateSearcher()
    $searcher2.ServerSelection = 2
    $updates2 = $searcher2.Search('IsInstalled=0').Updates
    $driverUpdates2 = $updates2 | Where-Object { $_.Title -match 'NVIDIA' -or $_.Title -match 'GeForce' }
    if ($driverUpdates2) {
        $driverUpdates2 | ForEach-Object { Write-Host $_.Title }
    } else {
        Write-Host 'No NVIDIA/display driver updates found via Microsoft Update'
    }
} catch {
    Write-Host "Error searching Microsoft Update: $($_.Exception.Message)"
}
