# Живая проверка z-порядка окон фото-галереи (read-only, ничего не двигает и не показывает).
#
# Пока идёт показ (есть окно PhotoStage), оверлеи — окно состояния GMagpie.GalleryStatus и
# список GMagpie.GalleryPicker — обязаны: (1) не иметь владельца (owned утягивается под
# перетверждающий z-порядок окно вывода), (2) быть в topmost-полосе, (3) находиться в
# z-порядке СТРОГО над окном вывода, иначе фото рисуется поверх меню/строки. Проверено
# зондом и скриншотом: инварианты по исходнику это поймать не могли.
#
# Код возврата: 0 — всё над фото; 1 — нарушено (тест красный); 2 — показа сейчас нет
# (GMagpie не запущен или галерея не идёт) — это не провал, а «нечего проверять».
#
# Запуск: pwsh -NoProfile -File tests\probe_photo_gallery_zorder.ps1   (пока открыт показ)
#
# Requires -Version 7.0
#Requires -PSEdition Core
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class WinZ {
    public class Row {
        public IntPtr Hwnd;
        public int ZIndex;
        public string Class = "";
        public string Title = "";
        public bool Visible;
        public bool Topmost;
        public IntPtr Owner;
        public int Pid;
        public int[] Rect; // left, top, right, bottom
    }

    delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int max);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern int GetWindowLongW(IntPtr h, int idx);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd); // GW_OWNER=4, GW_HWNDFIRST=0, GW_HWNDNEXT=2
    [DllImport("user32.dll")] static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);

    [StructLayout(LayoutKind.Sequential)] struct RECT { public int l, t, r, b; }

    // Истинное сравнение «кто выше в z-порядке»: обход цепочки GW_HWNDPREV от окна.
    // (EnumWindows не гарантирует порядок по z, а GetWindow(Desktop, GW_HWNDFIRST)
    // на этой системе возвращает NULL — цепочки от самого окна работают.)
    public static bool IsBehind(IntPtr lower, IntPtr upper) {
        var h = GetWindow(lower, 3); // GW_HWNDPREV = перед (выше) по z
        for (int i = 0; i < 2000 && h != IntPtr.Zero; i++, h = GetWindow(h, 3)) {
            if (h == upper) return true;
        }
        return false;
    }

    // Верхнеуровневые окна (порядок перечисления — приближение z, для печати).
    public static List<Row> TopDown() {
        var rows = new List<Row>();
        EnumWindows((h, _) => {
            var sb = new System.Text.StringBuilder(256);
            GetClassNameW(h, sb, 256);
            var tb = new System.Text.StringBuilder(256);
            GetWindowTextW(h, tb, 256);
            GetWindowThreadProcessId(h, out uint pid);
            var ex = GetWindowLongW(h, -20); // GWL_EXSTYLE (как int, не 0xD4)
            RECT rc = default;
            GetWindowRect(h, out rc);
            rows.Add(new Row {
                Hwnd = h,
                ZIndex = rows.Count,
                Class = sb.ToString(),
                Title = tb.ToString(),
                Visible = IsWindowVisible(h),
                Topmost = (ex & 0x8) != 0, // WS_EX_TOPMOST
                Owner = GetWindow(h, 4),
                Pid = (int)pid,
                Rect = new[] { rc.l, rc.t, rc.r, rc.b },
            });
            return true;
        }, IntPtr.Zero);
        return rows;
    }
}
'@

$SCALING_CLASS = 'Window_Magpie_967EB565-6F73-4E94-AE53-00CC42592A22'
$STAGE_CLASS = 'GMagpie.PhotoStage'
$STATUS_CLASS = 'GMagpie.GalleryStatus'
$PICKER_CLASS = 'GMagpie.GalleryPicker'

$rows = [WinZ]::TopDown()

# Окна только нашего GMagpie (F:\Projects\cpp\GMagpie\bin), не любой системы
$magpie = Get-Process GMagpie -ErrorAction SilentlyContinue | Where-Object {
    try { $_.Path -like 'F:\Projects\cpp\GMagpie\bin\*' } catch { $false }
}
if (-not $magpie) { Write-Output 'GMagpie (наш) не запущен — проверять нечего.'; exit 2 }
$pids = @($magpie.Id)
$mine = @($rows | Where-Object { $pids -contains $_.Pid })

$stage = @($mine | Where-Object { $_.Class -eq $STAGE_CLASS })
$scaling = @($mine | Where-Object { $_.Class -eq $SCALING_CLASS })
if (-not $stage -or -not $scaling) {
    $what = @()
    if (-not $stage) { $what += 'нет PhotoStage' }
    if (-not $scaling) { $what += 'нет окна вывода' }
    Write-Output "Фото-показ сейчас не идёт ($($what -join ', ')) — проверять нечего."
    exit 2
}

Write-Output '--- Окна GMagpie сверху вниз по z-порядку (0 = самое верхнее) ---'
foreach ($w in $mine) {
    $state = if ($w.Visible) { 'видно' } else { 'скрыто' }
    $tm = if ($w.Topmost) { 'topmost' } else { 'обычное' }
    $cls = if ($w.Class.Length -gt 40) { $w.Class.Substring(0, 40) + '...' } else { $w.Class }
    Write-Output ("z={0,-3} {1,-12} {2,-40} {3,-8} {4} owner={5}" -f $w.ZIndex, $state, $cls, $tm, $w.Hwnd, $w.Owner)
}

$failures = @()
$checked = 0
foreach ($name in @($STATUS_CLASS, $PICKER_CLASS)) {
    $all_of_class = @($mine | Where-Object { $_.Class -eq $name })
    $overlay = @($all_of_class | Where-Object { $_.Visible })
    if ($name -eq $STATUS_CLASS -and -not $overlay) {
        # строка состояния обязана быть видна весь показ — её исчезновение и есть баг
        $failures += "$name не видна во время показа (окно: $(if ($all_of_class) { 'есть, скрыто' } else { 'отсутствует' }))"
        continue
    }
    if (-not $overlay) { continue }  # список открыт не всегда; отсутствие — не нарушение
    $checked++
    foreach ($o in $overlay) {
        if ($o.Owner -ne [IntPtr]::Zero) {
            $failures += "$name имеет владельца ($($o.Owner)) — owned-окно утягивается под кадр"
        }
        if (-not $o.Topmost) {
            $failures += "$name вне topmost-полосы"
        }
        if ([WinZ]::IsBehind($o.Hwnd, $scaling[0].Hwnd)) {
            $failures += "$name ниже фото в z-порядке (цепочка GW_HWNDPREV ведёт от оверлея к окну вывода)"
        }
    }
}

if ($checked -eq 0) {
    if ($failures.Count -eq 0) {
        Write-Output 'Ни строка состояния, ни список сейчас не видны — проверять нечего.'
        exit 2
    }
}
if ($failures.Count -gt 0) {
    Write-Output '--- ПРОВАЛ ---'
    $failures | ForEach-Object { Write-Output "FAIL $_" }
    exit 1
}
Write-Output 'z-порядок: все видимые оверлеи owned выводом и находятся над фото.'
exit 0
