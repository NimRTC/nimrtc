# Try NVIDIA China mirror
$urls = @(
    'https://cn.download.nvidia.com/Windows/566.03/566.03-desktop-win10-win11-64bit-international-dch-whql.exe',
    'https://uk.download.nvidia.com/Windows/566.03/566.03-desktop-win10-win11-64bit-international-dch-whql.exe',
    'https://driver.prnvidia.com/Windows/566.03/566.03-desktop-win10-win11-64bit-international-dch-whql.exe'
)
$out = 'D:\MyOpen\NimRTC\temp_nvidia_driver.exe'
foreach ($url in $urls) {
    try {
        Write-Host "Trying: $url"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $headers = @{
            'User-Agent' = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'
            'Accept' = '*/*'
            'Referer' = 'https://www.nvidia.com/Download/index.aspx'
        }
        Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -TimeoutSec 30 -Headers $headers
        $size = (Get-Item $out).Length / 1MB
        if ($size -gt 100) {
            Write-Host "SUCCESS: Downloaded $size MB"
            break
        }
    } catch {
        Write-Host "Failed: $($_.Exception.Message)"
    }
}
