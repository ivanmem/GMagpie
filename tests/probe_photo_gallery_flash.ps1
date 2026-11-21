# Живая проба «вспышка пропорций»: при перелистывании 16:9 → 1:9... т.е. широкий кадр
# (2560x1440, красный) → квадрат (1440x1440, синий) НЕ должно быть ни одного
# отрисованного кадра со старым содержимым в новой геометрии (красное в квадрате)
# или с новым содержимым в старой (синее в широком), ни смешанных кадров.
# Серию скриншотов окна вывода сразу после → классифицируем покадрово.
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "bin\x64\Release\GMagpie.exe"
if (-not (Test-Path $exe)) { Write-Output "НЕТ СБОРКИ $exe"; exit 2 }

Add-Type -AssemblyName System.Drawing
$drawCommon = [AppDomain]::CurrentDomain.GetAssemblies() |
    Where-Object { $_.Location -like '*System.Drawing.Common.dll' } |
    Select-Object -First 1 -ExpandProperty Location
if (-not $drawCommon) { Write-Output 'НЕТ System.Drawing.Common'; exit 2 }
$drawDeps = 'System.Private.Windows.GdiPlus.dll', 'System.Private.Windows.Core.dll', 'System.Drawing.Primitives.dll' |
    ForEach-Object { Join-Path (Split-Path $drawCommon) $_ } | Where-Object { Test-Path $_ }
Add-Type -Language CSharp -ReferencedAssemblies (@($drawCommon) + $drawDeps) -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class F {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string wnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
    public static void Press(byte vk) { keybd_event(vk, 0, 0, UIntPtr.Zero); keybd_event(vk, 0, 2, UIntPtr.Zero); }
    // уменьшает до 320x180 и по маскам цветов считает прямоугольник содержимого
    public static string Analyze(Bitmap src) {
        var small = new Bitmap(320, 180, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(small)) {
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.Bilinear;
            g.DrawImage(src, 0, 0, 320, 180);
        }
        var d = small.LockBits(new Rectangle(0, 0, 320, 180), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        int rl = 320, rt = 180, rr = -1, rb = -1, rc = 0;
        int bl = 320, bt = 180, br = -1, bb = -1, bc = 0;
        byte[] buf = new byte[Math.Abs(d.Stride) * 180];
        Marshal.Copy(d.Scan0, buf, 0, buf.Length);
        for (int y = 0; y < 180; y++) {
            for (int x = 0; x < 320; x++) {
                int o = y * d.Stride + x * 3;
                int b = buf[o], gg = buf[o + 1], r = buf[o + 2];
                if (r > 80 && r > b + 30 && r > gg + 30) {
                    rc++; if (x < rl) rl = x; if (x > rr) rr = x; if (y < rt) rt = y; if (y > rb) rb = y;
                } else if (b > 80 && b > r + 30 && b > gg + 30) {
                    bc++; if (x < bl) bl = x; if (x > br) br = x; if (y < bt) bt = y; if (y > bb) bb = y;
                }
            }
        }
        small.UnlockBits(d); small.Dispose(); src.Dispose();
        var sb = new System.Text.StringBuilder();
        sb.Append("red=").Append(rc);
        if (rc > 64 && rr > rl) sb.Append(":AR=").Append(((rr - rl + 1) / (double)(rb - rt + 1)).ToString("F2", System.Globalization.CultureInfo.InvariantCulture));
        sb.Append(" blue=").Append(bc);
        if (bc > 64 && br > bl) sb.Append(":AB=").Append(((br - bl + 1) / (double)(bb - bt + 1)).ToString("F2", System.Globalization.CultureInfo.InvariantCulture));
        return sb.ToString();
    }
}
'@

# --- тестовый альбом: красный 16:9, затем синий квадрат ---
$album = Join-Path $env:TEMP "opencode\flash_album"
if (Test-Path $album) { Remove-Item -LiteralPath $album -Recurse -Force }
New-Item -ItemType Directory -Path $album | Out-Null
Add-Type -AssemblyName System.Drawing
foreach ($spec in @(@('a_red_wide.png', 2560, 1440, [System.Drawing.Color]::FromArgb(255, 220, 20, 60)),
                   @('b_blue_square.png', 1440, 1440, [System.Drawing.Color]::FromArgb(255, 30, 60, 230)))) {
    $bmp = New-Object System.Drawing.Bitmap([int]$spec[1], [int]$spec[2])
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear($spec[3]); $g.Dispose()
    $bmp.Save((Join-Path $album $spec[0]), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$ours = Get-Process GMagpie -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -like "$repo\bin\*" } catch { $false } }
if ($ours) { $ours | Stop-Process -Force; Start-Sleep -Seconds 2 }
# лёгкий показ: свой мьютекс, не мешает оригинальному GMagpie
[void](Start-Process $exe -ArgumentList '--gallery', "`"$album`"" -PassThru)
[void][F]::SetProcessDPIAware()   # иначе GetWindowRect/CopyFromScreen режут кадр до виртуального размера
$out = [IntPtr]::Zero
for ($i = 0; $i -lt 25; $i++) { Start-Sleep -Seconds 1; $out = [F]::FindWindowW('Window_Magpie_967EB565-6F73-4E94-AE53-00CC42592A22', $null); if ($out -ne [IntPtr]::Zero) { break } }
if ($out -eq [IntPtr]::Zero) { Write-Output 'НЕТ СЕССИИ'; exit 2 }
Start-Sleep -Seconds 5   # первая фотография + инициализация VSR
[F]::Press(0x12); [void][F]::SetForegroundWindow($out)
Start-Sleep -Milliseconds 400

# --- серии до/после перелистывания ---
$rect = New-Object 'F+RECT'
[void][F]::GetWindowRect($out, [ref]$rect)
$x, $y, $w, $h = $rect.l, $rect.t, ($rect.r - $rect.l), ($rect.b - $rect.t)
Write-Output "окно вывода: $x,$y ${w}x$h"
function Grab() {
    $bmp = New-Object System.Drawing.Bitmap([Math]::Max(10, $w), [Math]::Max(10, $h))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
    $g.Dispose()
    return $bmp
}
$t0 = Get-Date
$frames = @()
function Snap($tag) {
    $script:frames += [pscustomobject]@{
        t   = [int](Get-Date).Subtract($t0).TotalMilliseconds
        tag = $tag
        s   = [F]::Analyze((Grab))
    }
}
Snap 'before1'; Snap 'before2'
[F]::Press(0x27)   # следующая фотография (квадрат)
$deadline = (Get-Date).AddSeconds(6)
$stableBlue = 0
while ((Get-Date) -lt $deadline) {
    Snap 'burst'
    $last = $frames[-1].s
    if ($last -match 'blue=(\d+):AB=([\d.]+)' -and [int]$Matches[1] -gt 5000 -and [double]$Matches[2] -lt 1.35) {
        $stableBlue++
        if ($stableBlue -ge 4) { break }   # новая фотография села — серию можно кончать
    } else { $stableBlue = 0 }
}
[F]::Press(0x1B)   # остановить показ
Start-Sleep -Milliseconds 600
[F]::Press(0x1B)
Start-Sleep -Milliseconds 600
# не оставлять нашего GMagpie висеть в трее — только свой bin, чужой не трогаем
Get-Process GMagpie -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -like "$repo\bin\*" } catch { $false } } | Stop-Process -Force

# --- разбор ---
Write-Output "--- кадры ---"
foreach ($f in $frames) { Write-Output ("{0,6} мс {1,-8} {2}" -f $f.t, $f.tag, $f.s) }
$seenRedWide = $false; $seenBlueSquare = $false
$artifacts = @()
$blueFrames = @()
foreach ($f in $frames) {
    $s = $f.s
    $redN = 0; $redA = 0.0; $blueN = 0; $blueA = 0.0
    if ($s -match 'red=(\d+)(?::AR=([\d.]+))?') { $redN = [int]$Matches[1]; if ($Matches[2]) { $redA = [double]$Matches[2] } }
    if ($s -match 'blue=(\d+)(?::AB=([\d.]+))?') { $blueN = [int]$Matches[1]; if ($Matches[2]) { $blueA = [double]$Matches[2] } }
    $isRed = $redN -gt 5000; $isBlue = $blueN -gt 5000
    if ($isRed -and -not $isBlue) {
        if ($redA -ge 1.35) { $seenRedWide = $true }
        else { $artifacts += "кадр $($f.t) мс: СТАРОЕ фото (красное) с НОВЫМИ пропорциями (аспект $redA)" }
    } elseif ($isBlue -and -not $isRed) {
        if ($blueA -gt 0 -and $blueA -le 1.35) { if (-not $seenRedWide) { $artifacts += "кадр $($f.t) мс: квадрат раньше широкого?? порядок странный" }; $seenBlueSquare = $true }
        elseif ($blueA -gt 1.35) { $artifacts += "кадр $($f.t) мс: НОВОЕ фото (синее) СО СТАРЫМИ пропорциями (аспект $blueA)" }
        $blueFrames += $f.t
    } elseif ($isBlue -and $isRed) {
        $artifacts += "кадр $($f.t) мс: смешанный кадр (оба фото одновременно, red=$redN blue=$blueN)"
    }
}
Write-Output '--- итог ---'
if (-not $seenRedWide -or -not $seenBlueSquare) {
    Write-Output "FAIL-INVALID: нет перехода красный-широкий -> синий-квадрат (red=$seenRedWide blue=$seenBlueSquare)"
    exit 1
}
if ($artifacts.Count) {
    $artifacts | ForEach-Object { Write-Output "ВСПЫШКА: $_" }
    Write-Output "FAIL: кадр со сменой геометрии раньше содержимого (или наоборот) — $($( $artifacts | Select-Object -First 1))"
    exit 1
}
Write-Output "PASS: перелистывание без вспышки пропорций ($($blueFrames[0]) мс — первый кадр нового фото)"
exit 0
