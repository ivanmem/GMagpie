# Два Magpie рядом: обычный экземпляр (хоткеи, трей) + лёгкий показ с --gallery.
# Ожидание: оба живут, показ стартует, повторный --gallery блокируется только
# внутри своей группы. Красный на сборке без лёгкого режима: вторая копия выходит
# сразу («已经有一个实例正在运行»), окна показа нет.
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "bin\x64\Release\Magpie.exe"
if (-not (Test-Path $exe)) { Write-Output "НЕТ СБОРКИ $exe"; exit 2 }

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class C {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string wnd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void Press(byte vk) { keybd_event(vk, 0, 0, UIntPtr.Zero); keybd_event(vk, 0, 2, UIntPtr.Zero); }
}
'@

$album = Join-Path $env:TEMP "opencode\coexist_album"
if (-not (Test-Path -LiteralPath $album)) { New-Item -ItemType Directory -Path $album | Out-Null }
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap(1440, 1440)
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::FromArgb(255, 30, 160, 90)); $g.Dispose()
$png = (Join-Path -Path $album -ChildPath "one.png")
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()

Get-Process Magpie -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -like "$repo\bin\*" } catch { $false } } | Stop-Process -Force
Start-Sleep -Seconds 2

# 1) обычный экземпляр — сразу в трей (-t), экран не трогает
$normal = Start-Process $exe -ArgumentList '-t' -PassThru
Start-Sleep -Seconds 4
if ($normal.HasExited) { Write-Output "FAIL: обычный экземпляр сразу вышел (код $($normal.ExitCode))"; exit 1 }
Write-Output "обычный экземпляр жив (pid $($normal.Id))"

# 2) лёгкий показ рядом
$gallery = Start-Process $exe -ArgumentList '--gallery', "`"$album`"" -PassThru
$found = [IntPtr]::Zero
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Seconds 1
    $found = [C]::FindWindowW('Window_Magpie_967EB565-6F73-4E94-AE53-00CC42592A22', $null)
    if ($found -ne [IntPtr]::Zero) { break }
}
Start-Sleep -Seconds 2
$normalAlive = -not $normal.HasExited
$galleryAlive = -not $gallery.HasExited
Write-Output "окно показа: $($found -ne [IntPtr]::Zero); обычный жив: $normalAlive; показ жив: $galleryAlive"

# 3) повторный --gallery — блокируется только внутри своей группы
$dup = Start-Process $exe -ArgumentList '--gallery', "`"$album`"" -PassThru
$dupBlocked = $dup.WaitForExit(6000)

# остановим показ (Esc по окну) и приберём за собой оба процесса
if ($found -ne [IntPtr]::Zero) { [C]::Press(0x1B); Start-Sleep -Milliseconds 800; [C]::Press(0x1B) }
Start-Sleep -Seconds 1
foreach ($p in @($normal, $gallery, $dup)) {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
Get-Process Magpie -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -like "$repo\bin\*" } catch { $false } } | Stop-Process -Force

Write-Output '--- итог ---'
$fail = @()
if (-not ($found -ne [IntPtr]::Zero)) { $fail += 'показ не запустился рядом с обычным экземпляром (общий мьютекс?)' }
if (-not $normalAlive) { $fail += 'обычный экземпляр умер при запуске показа (вытеснение SHOWME/QUIT?)' }
if (-not $dupBlocked) { $fail += 'повторный --gallery не заблокировался внутри своей группы' }
if ($fail.Count) { $fail | ForEach-Object { Write-Output "FAIL: $_" }; exit 1 }
Write-Output "PASS: лёгкий показ уживается с обычным экземпляром; повторный показ блокируется только в своей группе"
exit 0
