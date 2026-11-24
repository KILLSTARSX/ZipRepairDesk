param(
  [string]$Path = (Join-Path $PSScriptRoot "dist\ZipRepairDesk.exe"),
  [string]$Thumbprint = $env:CODESIGN_THUMBPRINT
)
$ErrorActionPreference = "Stop"
if (-not $Thumbprint) { throw "Pass -Thumbprint <sha1> or set CODESIGN_THUMBPRINT." }
$Certificate = Get-ChildItem Cert:\CurrentUser\My |
  Where-Object { $_.Thumbprint -eq $Thumbprint -and $_.HasPrivateKey } |
  Sort-Object NotAfter -Descending |
  Select-Object -First 1
if (-not $Certificate) { throw "Code-signing certificate with private key not found: $Thumbprint" }
$Result = Set-AuthenticodeSignature -FilePath $Path -Certificate $Certificate -HashAlgorithm SHA256
if ($Result.Status -ne "Valid") { throw "Authenticode signing failed: $($Result.Status) $($Result.StatusMessage)" }
$File = Get-Item $Path
[PSCustomObject]@{
  File = $File.FullName
  Length = $File.Length
  SHA256 = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLowerInvariant()
  Signature = $Result.Status
  Signer = $Result.SignerCertificate.Subject
}
