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
    Write-Host "Convert OpenVDB .vdb files to NanoVDB .nvdb files."
    Write-Host ""
    Write-Host "Usage:"
    Write-Host "  tools\convert_vdb_to_nvdb.ps1 assets\smoke.vdb"
    Write-Host "  tools\convert_vdb_to_nvdb.ps1 assets\smoke.vdb assets\smoke.nvdb"
    Write-Host "  tools\convert_vdb_to_nvdb.ps1 assets\vdbs assets\nvdbs -Force"
    Write-Host "  tools\convert_vdb_to_nvdb.ps1 assets\smoke.vdb -ConverterPath C:\openvdb\bin\nanovdb_convert.exe"
    Write-Host ""
    Write-Host "Expected converter:"
    Write-Host "  nanovdb_convert.exe from OpenVDB/NanoVDB."
    Write-Host ""
    Write-Host "Search order:"
    Write-Host "  -ConverterPath"
    Write-Host "  tools\nanovdb_convert.exe"
    Write-Host "  external\openvdb\bin\nanovdb_convert.exe"
    Write-Host "  external\nanovdb\bin\nanovdb_convert.exe"
    Write-Host "  PATH"
}

function Repo-Root {
    $scriptDir = Split-Path -Parent $PSCommandPath
    return (Resolve-Path (Join-Path $scriptDir "..")).Path
}

function Resolve-Converter {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        if (!(Test-Path -LiteralPath $ExplicitPath)) {
            throw "Converter not found: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $root = Repo-Root
    $candidates = @(
        (Join-Path $root "tools\nanovdb_convert.exe"),
        (Join-Path $root "external\openvdb\bin\nanovdb_convert.exe"),
        (Join-Path $root "external\nanovdb\bin\nanovdb_convert.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $cmd = Get-Command "nanovdb_convert.exe" -ErrorAction SilentlyContinue
    if (!$cmd) {
        $cmd = Get-Command "nanovdb_convert" -ErrorAction SilentlyContinue
    }
    if ($cmd) {
        return $cmd.Source
    }

    throw "nanovdb_convert was not found. Build/install OpenVDB NanoVDB tools, put nanovdb_convert.exe in tools\, or pass -ConverterPath."
}

function Collect-Inputs {
    param([string[]]$Paths)

    $files = New-Object System.Collections.Generic.List[string]
    foreach ($path in $Paths) {
        if (!(Test-Path -LiteralPath $path)) {
            throw "Input path not found: $path"
        }

        $item = Get-Item -LiteralPath $path
        if ($item.PSIsContainer) {
            Get-ChildItem -LiteralPath $item.FullName -Filter "*.vdb" -File | ForEach-Object {
                $files.Add($_.FullName)
            }
        } else {
            if ([System.IO.Path]::GetExtension($item.FullName).ToLowerInvariant() -ne ".vdb") {
                throw "Input file is not a .vdb: $($item.FullName)"
            }
            $files.Add($item.FullName)
        }
    }

    if ($files.Count -eq 0) {
        throw "No .vdb files found."
    }
    return $files.ToArray()
}

function Resolve-Output-Path {
    param(
        [string]$InputFile,
        [string]$OutputArg,
        [int]$InputCount
    )

    if (!$OutputArg) {
        $dir = Split-Path -Parent $InputFile
        $name = [System.IO.Path]::GetFileNameWithoutExtension($InputFile) + ".nvdb"
        return Join-Path $dir $name
    }

    $outputLooksLikeFile = [System.IO.Path]::GetExtension($OutputArg).ToLowerInvariant() -eq ".nvdb"
    if ($InputCount -eq 1 -and $outputLooksLikeFile) {
        $outDir = Split-Path -Parent $OutputArg
        if ($outDir -and !(Test-Path -LiteralPath $outDir)) {
            New-Item -ItemType Directory -Path $outDir | Out-Null
        }
        return $OutputArg
    }

    if (!(Test-Path -LiteralPath $OutputArg)) {
        New-Item -ItemType Directory -Path $OutputArg | Out-Null
    }

    $name = [System.IO.Path]::GetFileNameWithoutExtension($InputFile) + ".nvdb"
    return Join-Path $OutputArg $name
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
    if ($outDir -and !(Test-Path -LiteralPath $outDir)) {
        New-Item -ItemType Directory -Path $outDir | Out-Null
    }

    if ((Test-Path -LiteralPath $outFile) -and !$Force) {
        throw "Output exists: $outFile. Use -Force to overwrite."
    }

    $args = @($inputFile, $outFile)
    Write-Host "VDB -> NVDB: $inputFile"
    Write-Host "             $outFile"

    if ($DryRun) {
        Write-Host "Dry run: & `"$converter`" `"$inputFile`" `"$outFile`""
        continue
    }

    & $converter @args
    if ($LASTEXITCODE -ne 0) {
        throw "nanovdb_convert failed with exit code $LASTEXITCODE for $inputFile"
    }
}

Write-Host "Done."
