$ErrorActionPreference = "Stop"

$rootDir = Resolve-Path (Join-Path $PSScriptRoot "..")
$distDir = Join-Path $rootDir "dist"

$blenderZip = Join-Path $distDir "abcexport-blender-addon.zip"
$houdiniZip = Join-Path $distDir "abcexport-houdini-plugin-windows.zip"

if (Test-Path $distDir) {
    Remove-Item -LiteralPath $distDir -Recurse -Force
}
New-Item -ItemType Directory -Path $distDir | Out-Null

Write-Host "Building the converter..."
& (Join-Path $rootDir "converter\build.bat")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Packaging the Blender add-on..."
Get-ChildItem (Join-Path $rootDir "blender\abc_export") -Directory -Recurse -Filter "__pycache__" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force
Compress-Archive -Path (Join-Path $rootDir "blender\abc_export") -DestinationPath $blenderZip -Force

Write-Host "Packaging the Houdini plugin..."
$houdiniStage = Join-Path $distDir "abcexport-houdini-plugin"
New-Item -ItemType Directory -Path (Join-Path $houdiniStage "converter\bin") -Force | Out-Null
Copy-Item (Join-Path $rootDir "houdini\abc_export.1.0.hda") $houdiniStage
Copy-Item (Join-Path $rootDir "converter\mesh2abc.cpp") (Join-Path $houdiniStage "converter")
Copy-Item (Join-Path $rootDir "converter\CMakeLists.txt") (Join-Path $houdiniStage "converter")
Copy-Item (Join-Path $rootDir "converter\build.sh") (Join-Path $houdiniStage "converter")
Copy-Item (Join-Path $rootDir "converter\build.bat") (Join-Path $houdiniStage "converter")
Copy-Item (Join-Path $rootDir "converter\bin\mesh2abc.exe") (Join-Path $houdiniStage "converter\bin")
Copy-Item (Join-Path $rootDir "LICENSE") $houdiniStage
Compress-Archive -Path $houdiniStage -DestinationPath $houdiniZip -Force

Write-Host
Write-Host "Release packages ready:"
Write-Host "  $blenderZip"
Write-Host "  $houdiniZip"
