#Requires -Version 7.0
param([string]$Output)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$Output) { $Output = Join-Path $repo 'obj/tests/photo-gallery' }
New-Item -ItemType Directory -Path $Output -Force | Out-Null
# The repository convention is `python`; on machines where that is only the
# Microsoft Store alias it exits 9009, so fall back to the launcher.
& python -B (Join-Path $PSScriptRoot 'prepare_photo_gallery_test.py') $Output
if ($LASTEXITCODE) {
    & py -3 -B (Join-Path $PSScriptRoot 'prepare_photo_gallery_test.py') $Output
    if ($LASTEXITCODE) { throw 'Photo gallery logic extraction failed.' }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$Output" (Join-Path $PSScriptRoot 'PhotoGalleryTests.cpp') "/Fe:$Output/PhotoGalleryTests.exe" "/Fo:$Output/PhotoGalleryTests.obj"
if ($LASTEXITCODE) { throw 'Photo gallery logic test compilation failed.' }
& (Join-Path $Output 'PhotoGalleryTests.exe')
if ($LASTEXITCODE) { throw 'Photo gallery logic tests failed.' }
