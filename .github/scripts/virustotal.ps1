# virustotal.ps1 - send a file to VirusTotal (about 70 antivirus engines) and wait up to about 5 minutes for the result.
# Usage (GitHub Actions, pwsh): ./.github/scripts/virustotal.ps1 -Path build/mtpsetup.exe
# Needs $env:VT_KEY. Writes "link=..." and, when the analysis finished in time, "summary=..." to $env:GITHUB_OUTPUT.
# The public API allows 4 requests per minute, so it polls every 20 seconds.
param([Parameter(Mandatory = $true)][string]$Path)
$ErrorActionPreference = 'Stop'

if (-not $env:VT_KEY) { Write-Host 'VIRUSTOTAL_API_KEY is not set; skipped'; exit 0 }
$sha256 = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower()
$link = "https://www.virustotal.com/gui/file/$sha256"
"link=$link" >> $env:GITHUB_OUTPUT

$headers = @{ 'x-apikey' = $env:VT_KEY }
$upload = Invoke-RestMethod -Method Post -Uri 'https://www.virustotal.com/api/v3/files' -Headers $headers -Form @{ file = Get-Item $Path }
$id = $upload.data.id
Write-Host "VirusTotal analysis: $id"
for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Seconds 20
    $a = Invoke-RestMethod -Uri "https://www.virustotal.com/api/v3/analyses/$id" -Headers $headers
    if ($a.data.attributes.status -eq 'completed') {
        $s = $a.data.attributes.stats
        $total = $s.malicious + $s.suspicious + $s.undetected + $s.harmless
        Write-Host "malicious=$($s.malicious) suspicious=$($s.suspicious) undetected=$($s.undetected) harmless=$($s.harmless)"
        "summary=$(Get-Date -Format 'yyyy-MM-dd') 時点で $total のエンジン中、危険 $($s.malicious)・疑わしい $($s.suspicious)" >> $env:GITHUB_OUTPUT
        break
    }
}
Write-Host "VirusTotal: $link"
