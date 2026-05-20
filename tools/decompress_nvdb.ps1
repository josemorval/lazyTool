param(
    [Parameter(Position = 0)]
    [string[]]$InputPath,

    [Parameter(Position = 1)]
    [string]$Output,

    [string]$ConverterPath,
    [switch]$Force,
    [switch]$DryRun,
    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Show-Usage {
    Write-Host "Rewrite compressed NanoVDB .nvdb files as uncompressed .nvdb files."
    Write-Host ""
    Write-Host "Usage:"
    Write-Host "  tools\decompress_nvdb.bat assets\cloud_compressed.nvdb"
    Write-Host "  tools\decompress_nvdb.bat assets\cloud_compressed.nvdb assets\cloud_uncompressed.nvdb"
    Write-Host "  tools\decompress_nvdb.bat assets\nvdbs assets\nvdbs_uncompressed -Force"
    Write-Host "  tools\decompress_nvdb.bat assets\cloud.nvdb -ConverterPath C:\openvdb\bin\nanovdb_convert.exe"
    Write-Host ""
    Write-Host "This wrapper requires nanovdb_convert from OpenVDB/NanoVDB."
    Write-Host "It tries common no-compression flags used by NanoVDB builds."
}

function Repo-Root {
    $scriptDir = Split-Path -Parent $PSCommandPath
    return (Resolve-Path (Join-Path $scriptDir "..")).Path
}

function Resolve-Converter {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        if (!(Test-Path -LiteralPath $ExplicitPath)) { throw "Converter not found: $ExplicitPath" }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $root = Repo-Root
    $candidates = @(
        (Join-Path $root "tools\nanovdb_convert.exe"),
        (Join-Path $root "external\openvdb\bin\nanovdb_convert.exe"),
        (Join-Path $root "external\nanovdb\bin\nanovdb_convert.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }

    $cmd = Get-Command "nanovdb_convert.exe" -ErrorAction SilentlyContinue
    if (!$cmd) { $cmd = Get-Command "nanovdb_convert" -ErrorAction SilentlyContinue }
    if ($cmd) { return $cmd.Source }
    throw "nanovdb_convert was not found. Put it in tools\, install OpenVDB tools, or pass -ConverterPath."
}

function Collect-Inputs {
    param([string[]]$Paths)
    $files = New-Object System.Collections.Generic.List[string]
    foreach ($path in $Paths) {
        if (!(Test-Path -LiteralPath $path)) { throw "Input path not found: $path" }
        $item = Get-Item -LiteralPath $path
        if ($item.PSIsContainer) {
            Get-ChildItem -LiteralPath $item.FullName -Filter "*.nvdb" -File | ForEach-Object { $files.Add($_.FullName) }
        } else {
            if ([System.IO.Path]::GetExtension($item.FullName).ToLowerInvariant() -ne ".nvdb") {
                throw "Input file is not a .nvdb: $($item.FullName)"
            }
            $files.Add($item.FullName)
        }
    }
    if ($files.Count -eq 0) { throw "No .nvdb files found." }
    return $files.ToArray()
}

function Resolve-Output-Path {
    param([string]$InputFile, [string]$OutputArg, [int]$InputCount)

    if (!$OutputArg) {
        $dir = Split-Path -Parent $InputFile
        $name = [System.IO.Path]::GetFileNameWithoutExtension($InputFile) + "_uncompressed.nvdb"
        return Join-Path $dir $name
    }

    $outputLooksLikeFile = [System.IO.Path]::GetExtension($OutputArg).ToLowerInvariant() -eq ".nvdb"
    if ($InputCount -eq 1 -and $outputLooksLikeFile) {
        $outDir = Split-Path -Parent $OutputArg
        if ($outDir -and !(Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
        return $OutputArg
    }

    if (!(Test-Path -LiteralPath $OutputArg)) { New-Item -ItemType Directory -Path $OutputArg | Out-Null }
    $name = [System.IO.Path]::GetFileNameWithoutExtension($InputFile) + "_uncompressed.nvdb"
    return Join-Path $OutputArg $name
}

function Invoke-ConverterNoCompression {
    param([string]$Converter, [string]$InputFile, [string]$OutFile)

    $attempts = @(
        @("-compression", "none", $InputFile, $OutFile),
        @("--compression", "none", $InputFile, $OutFile),
        @("-codec", "none", $InputFile, $OutFile),
        @("--codec", "none", $InputFile, $OutFile),
        @($InputFile, $OutFile)
    )

    foreach ($args in $attempts) {
        Write-Host "Trying: $Converter $($args -join ' ')"
        if ($DryRun) { continue }
        & $Converter @args
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $OutFile)) { return }
    }
    throw "nanovdb_convert could not rewrite $InputFile as uncompressed .nvdb."
}

if ($Help -or !$InputPath -or $InputPath.Count -eq 0) {
    Show-Usage
    exit 0
}

$converter = Resolve-Converter -ExplicitPath $ConverterPath
$inputs = Collect-Inputs -Paths $InputPath
Write-Host "Converter: $converter"
Write-Host "Inputs:    $($inputs.Count)"

foreach ($inputFile in $inputs) {
    $outFile = Resolve-Output-Path -InputFile $inputFile -OutputArg $Output -InputCount $inputs.Count
    $outDir = Split-Path -Parent $outFile
    if ($outDir -and !(Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
    if ((Test-Path -LiteralPath $outFile) -and !$Force) {
        throw "Output exists: $outFile. Use -Force to overwrite."
    }
    if ((Test-Path -LiteralPath $outFile) -and $Force) {
        Remove-Item -LiteralPath $outFile -Force
    }

    Write-Host "NVDB compressed -> uncompressed: $inputFile"
    Write-Host "                                $outFile"
    Invoke-ConverterNoCompression -Converter $converter -InputFile $inputFile -OutFile $outFile
}

Write-Host "Done."
