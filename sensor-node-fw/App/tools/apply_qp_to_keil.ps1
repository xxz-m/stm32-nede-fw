<#
-------------------------------------------------------------------------------
 apply_qp_to_keil.ps1 —— 把 QP/C 接入相关的设置补回 CubeMX 生成的 Keil 工程

 背景：每次在 STM32CubeMX 里点 GENERATE CODE，<工程>.uvprojx 都会被重写，
       手工加入的文件组、预定义宏、头文件路径会丢失（ST 社区已确认）。
       手写代码都在 App/ 与 Qpc/ 下，CubeMX 不会删除，但需要重新挂回工程。

 用法（幂等，可反复运行）：

   powershell -NoProfile -ExecutionPolicy Bypass -File App\tools\apply_qp_to_keil.ps1

   指定工程文件时：

   powershell -NoProfile -ExecutionPolicy Bypass -File App\tools\apply_qp_to_keil.ps1 -Project "C:\路径\X.uvprojx"

 做四件事：
   1. 补 <uAC6>1</uAC6>（ARM Compiler 6）
   2. 给预定义宏追加 QV_PRJ
   3. 重建头文件搜索路径：剔除已不存在的 App/Qpc/ThirdParty 路径，按磁盘实际目录补齐
   4. 重建文件分组：App / Service / Driver / BSP / ThirdParty/<库> / QP/C/platform / QP/C/port
      分组内容按磁盘上的 .c 自动发现，因此新增 .c 文件不需要修改本脚本
-------------------------------------------------------------------------------
#>
param(
    [string]$Project = ''
)

$ErrorActionPreference = 'Stop'

# --------------------------------------------------------------- 定位工程文件
if ([string]::IsNullOrWhiteSpace($Project)) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path      # <工程>\App\tools
    $rootGuess = Split-Path -Parent (Split-Path -Parent $scriptDir)   # <工程>
    $mdk = Join-Path $rootGuess 'MDK-ARM'
    if (-not (Test-Path $mdk)) { throw ("未找到 MDK-ARM 目录：" + $rootGuess) }
    $cand = @(Get-ChildItem -Path $mdk -Filter *.uvprojx -ErrorAction SilentlyContinue)
    if ($cand.Count -eq 0) { throw ("MDK-ARM 下没有 .uvprojx：" + $mdk) }
    if ($cand.Count -gt 1) { throw ("MDK-ARM 下有多个 .uvprojx，请用 -Project 指定：" + $mdk) }
    $Project = $cand[0].FullName
}
if (-not (Test-Path $Project)) { throw ("工程文件不存在：" + $Project) }

$projDir = Split-Path -Parent $Project
$root    = Split-Path -Parent $projDir
if (-not (Test-Path (Join-Path $root 'Core\Src'))) { throw ("工程根目录定位失败：" + $root) }

Write-Output 'QP/C 工程设置修补'
Write-Output ('  工程文件：' + $Project)
Write-Output ('  工程根目录：' + $root)
Write-Output ''

# ------------------------------------------------------------------ 工具函数
function Get-CFiles([string]$dir) {
    if (-not (Test-Path $dir)) { return @() }
    return @(Get-ChildItem $dir -Recurse -Filter *.c -File |
            Select-Object -ExpandProperty FullName | Sort-Object)
}
function Test-HasHeader([string]$dir) {
    if (-not (Test-Path $dir)) { return $false }
    return (@(Get-ChildItem $dir -Recurse -Filter *.h -File).Count -gt 0)
}
function Get-RelPath([string]$full) {
    $r = $full.Substring($root.Length).TrimStart('\')
    return ('../' + ($r -replace '\\', '/'))
}
function Get-CFileXml([string]$full, [string]$indent) {
    $name = [System.IO.Path]::GetFileName($full)
    return ($indent + "<File>`r`n" +
            $indent + "  <FileName>" + $name + "</FileName>`r`n" +
            $indent + "  <FileType>1</FileType>`r`n" +
            $indent + "  <FilePath>" + (Get-RelPath $full) + "</FilePath>`r`n" +
            $indent + "</File>`r`n")
}

$text = [System.IO.File]::ReadAllText($Project)
$originalText = $text
$changed = $false
$report = @()

# ------------------------------------------------------- 1) ARM Compiler 6（uAC6）
if ($text.IndexOf('<uAC6>') -lt 0) {
    $anchor = '<ToolsetName>ARM-ADS</ToolsetName>'
    $i = $text.IndexOf($anchor)
    if ($i -lt 0) { throw '未找到 <ToolsetName>ARM-ADS</ToolsetName>，这似乎不是 MDK 工程文件' }
    $text = $text.Insert($i + $anchor.Length, "`r`n      <uAC6>1</uAC6>")
    $changed = $true
    $report += '已补：<uAC6>1</uAC6>（ARM Compiler 6）'
}
else { $report += '无需处理：uAC6 已存在' }

# --------------------------------------------------------------- 2) 预定义宏
$script:defFixed = $false
$text = [regex]::Replace($text, '<Define>([^<]*)</Define>', {
        param($m)
        $v = $m.Groups[1].Value
        if (($v -match 'USE_HAL_DRIVER') -and ($v -notmatch 'QV_PRJ')) {
            $script:defFixed = $true
            return ('<Define>' + $v + ',QV_PRJ</Define>')
        }
        return $m.Value
    })
if ($script:defFixed) { $changed = $true; $report += '已补：预定义宏 QV_PRJ' }
else { $report += '无需处理：QV_PRJ 已存在' }

# ----------------------------------------------------------- 3) 头文件搜索路径
# 按磁盘实际情况收集需要加入编译的头文件目录
$incDirs = @()
foreach ($layer in @('App\app', 'App\app_command', 'App\service', 'App\driver', 'App\bsp', 'App\component')) {
    $d = Join-Path $root $layer
    if (Test-HasHeader $d) {
        $incDirs += (Get-RelPath $d)
        foreach ($headerDir in @(Get-ChildItem $d -Recurse -Filter *.h -File |
                                  Select-Object -ExpandProperty DirectoryName -Unique)) {
            $incDirs += (Get-RelPath $headerDir)
        }
    }
}
foreach ($d in @((Join-Path $root 'Qpc\platform\inc'))) {
    if (Test-HasHeader $d) { $incDirs += (Get-RelPath $d) }
}
foreach ($p in @(Get-ChildItem (Join-Path $root 'Qpc\port') -Directory -ErrorAction SilentlyContinue)) {
    $d = Join-Path $p.FullName 'inc'
    if (Test-HasHeader $d) { $incDirs += (Get-RelPath $d) }
}
foreach ($lib in @(Get-ChildItem (Join-Path $root 'ThirdParty') -Directory -ErrorAction SilentlyContinue)) {
    foreach ($sub in @($lib.FullName, (Join-Path $lib.FullName 'inc'))) {
        if (Test-HasHeader $sub) { $incDirs += (Get-RelPath $sub) }
    }
}
$incDirs = @($incDirs | Select-Object -Unique)

$script:incFixed = $false
$text = [regex]::Replace($text, '<IncludePath>([^<]*)</IncludePath>', {
        param($m)
        $v = $m.Groups[1].Value
        if ($v -notmatch 'Core/Inc') { return $m.Value }   # 只处理 C/C++ 的那一条
        # 先剔除指向已不存在目录的自研路径（CubeMX 重生成后可能残留旧路径）
        $keep = @()
        foreach ($p in ($v -split ';')) {
            if ($p -eq '') { continue }
            if ($p -match '(App|Qpc|ThirdParty)/') {
                $abs = Join-Path $root (($p -replace '^\.\./', '') -replace '/', '\')
                if (-not (Test-Path $abs)) { continue }
            }
            $keep += $p
        }
        $new = @($keep + $script:incDirs | Select-Object -Unique) -join ';'
        $script:incFixed = $true
        return ('<IncludePath>' + $new + '</IncludePath>')
    })
if ($script:incFixed) { $changed = $true; $report += ('已重建头文件路径，共 ' + $incDirs.Count + ' 条自研/第三方目录') }

# --------------------------------------------------------------- 4) 文件分组
# 每个分组按磁盘上的 .c 自动生成；同名分组先删除再重建，保证自愈
$groupDefs = @()
foreach ($g in @(
        @{ n = 'App';     d = 'App\app' },
        @{ n = 'Service'; d = 'App\service' },
        @{ n = 'Driver';  d = 'App\driver' },
        @{ n = 'BSP';     d = 'App\bsp' },
        @{ n = 'Component'; d = 'App\component' })) {
    $groupDefs += @{ name = $g.n; files = @(Get-CFiles (Join-Path $root $g.d)) }
}
$qpPlat = @(Get-ChildItem (Join-Path $root 'Qpc\platform\src') -Filter *.c -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notin @('qk.c', 'qxk.c', 'qxk_mutex.c', 'qxk_sema.c', 'qxk_xthr.c') } |
    Select-Object -ExpandProperty FullName | Sort-Object)
$groupDefs += @{ name = 'QP/C/platform'; files = $qpPlat }
$qpPort = @(Get-ChildItem (Join-Path $root 'Qpc\port') -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Get-CFiles $_.FullName } | Sort-Object)
$groupDefs += @{ name = 'QP/C/port'; files = $qpPort }
foreach ($lib in @(Get-ChildItem (Join-Path $root 'ThirdParty') -Directory -ErrorAction SilentlyContinue)) {
    $cf = @(Get-CFiles $lib.FullName)
    if ($cf.Count -gt 0) { $groupDefs += @{ name = ('ThirdParty/' + $lib.Name); files = $cf } }
}

$xml = ''
$added = 0
foreach ($g in $groupDefs) {
    if ($g.files.Count -eq 0) { continue }
    $pattern = '(?s)\s*<Group>\s*<GroupName>' + [regex]::Escape($g.name) + '</GroupName>.*?</Group>'
    $text = [regex]::Replace($text, $pattern, '')
    $xml += ("        <Group>`r`n          <GroupName>" + $g.name + "</GroupName>`r`n          <Files>`r`n")
    foreach ($f in $g.files) { $xml += (Get-CFileXml $f '            ') }
    $xml += "          </Files>`r`n        </Group>`r`n"
    $added++
    $report += ('已重建分组：' + $g.name + '（' + $g.files.Count + ' 个文件）')
}
if ($added -gt 0) {
    # 固定结束标记缩进，并在缩进前插入，避免重复运行时首组缩进来回变化。
    $text = [regex]::Replace($text, '\s*</Groups>', "`r`n      </Groups>")
    $closeIdx = $text.LastIndexOf('      </Groups>')
    if ($closeIdx -lt 0) { throw '未找到 </Groups> 结束标记' }
    $text = $text.Insert($closeIdx, $xml)
    $changed = $true
}

# -------------------- 4.5) After Build 生成 .bin（fromelf）
# Keil 只产出 .axf（以及勾选后的 .hex）；.bin 必须靠 after-build 命令生成。
# 这里固定写入 Run #1，烧写工具与 OTA 都用得着；CubeMX 重新生成工程后会丢，由本步骤补回。
# NOTE: 行尾必须是 CRLF，与工程文件其余部分保持一致。
$binCmd = 'fromelf --bin --output "$L@L.bin" "#L"'
$afterMakeNew = '<AfterMake>' + "`r`n" +
    '            <RunUserProg1>1</RunUserProg1>' + "`r`n" +
    '            <RunUserProg2>0</RunUserProg2>' + "`r`n" +
    '            <UserProg1Name>' + $binCmd + '</UserProg1Name>' + "`r`n" +
    '            <UserProg2Name></UserProg2Name>' + "`r`n" +
    '            <UserProg1Dos16Mode>0</UserProg1Dos16Mode>' + "`r`n" +
    '            <UserProg2Dos16Mode>0</UserProg2Dos16Mode>' + "`r`n" +
    '            <nStopA1X>0</nStopA1X>' + "`r`n" +
    '            <nStopA2X>0</nStopA2X>' + "`r`n" +
    '          </AfterMake>'
$afterMakeMatches = [regex]::Matches($text, '(?s)<AfterMake>.*?</AfterMake>')
if ($afterMakeMatches.Count -eq 0) { throw '未找到 <AfterMake> 段' }
$binTouched = 0
for ($i = $afterMakeMatches.Count - 1; $i -ge 0; $i--) {
    $mm = $afterMakeMatches[$i]
    if ($mm.Value -ne $afterMakeNew) {
        $text = $text.Remove($mm.Index, $mm.Length).Insert($mm.Index, $afterMakeNew)
        $changed = $true
        $binTouched++
    }
}
if ($binTouched -gt 0) {
    $report += ('已写入 After Build 命令，涉及 ' + $binTouched + ' 个 target（fromelf 生成 .bin）')
}

# ---------------------------------------------------------------------- 写回
# NOTE (工程换行)：源工程可能被编辑器改为 LF；统一 CRLF，避免与新插入段混用。
$crlfText = $text -replace "\r?\n", "`r`n"
if (-not [string]::Equals($crlfText, $text, [StringComparison]::Ordinal)) {
    $text = $crlfText
    $changed = $true
    $report += '已统一工程文件换行为 CRLF'
}
# 分组重新计算不代表文件真的改变；按字节对应的字符串内容决定是否写回。
$changed = -not [string]::Equals($originalText, $text, [StringComparison]::Ordinal)
if ($changed) {
    [System.IO.File]::WriteAllText($Project, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Output '--- 已修改工程文件 ---'
}
else {
    Write-Output '--- 无需修改（工程设置已是最新） ---'
}
$report | ForEach-Object { Write-Output $_ }

# ------------------------------- 5) 统一含中文源码的编码（UTF-8 BOM）
# 说明：Keil µVision 靠 BOM 判断源文件是 UTF-8。CubeMX 重新生成 main.c、it 等
# 文件时会丢掉 BOM（中文注释仍在），导致编辑器里显示乱码。这里统一补回 BOM。
$bomFixed = 0
$bomTargets = @()
$bomTargets += @(Get-ChildItem (Join-Path $root 'Core\Src') -Filter *.c -File -ErrorAction SilentlyContinue)
$bomTargets += @(Get-ChildItem (Join-Path $root 'App') -Recurse -Include *.c, *.h, *.ps1 -File -ErrorAction SilentlyContinue)
$bomTargets += @(Get-ChildItem (Join-Path $root 'Qpc') -Recurse -Include *.c, *.h -File -ErrorAction SilentlyContinue)
foreach ($f in $bomTargets) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    if ($hasBom) { continue }
    $txt = [System.IO.File]::ReadAllText($f.FullName)
    $nonAscii = @($txt.ToCharArray() | Where-Object { [int]$_ -gt 127 }).Count
    if ($nonAscii -gt 0) {
        [System.IO.File]::WriteAllText($f.FullName, $txt, (New-Object System.Text.UTF8Encoding($true)))
        $bomFixed++
    }
}
Write-Output ''
Write-Output ('编码统一：已为 ' + $bomFixed + ' 个含中文的源文件补上 UTF-8 BOM')

# ---------------------------------------------------------------------- 校验
Write-Output ''
Write-Output '--- 校验 ---'
$xmlDoc = New-Object System.Xml.XmlDocument
$xmlDoc.Load($Project)
$targets = @($xmlDoc.SelectNodes('//Target'))
Write-Output ('工程 XML 合法，target 数量 = ' + $targets.Count)

$missing = 0
$checked = 0
foreach ($f in $xmlDoc.SelectNodes('//FilePath')) {
    $rel = $f.InnerText
    $checked++
    if ($rel -match '^\.\./') {
        $full = Join-Path $root (($rel -replace '^\.\./', '') -replace '/', '\')
    }
    else {
        $full = Join-Path $projDir ($rel -replace '/', '\')
    }
    if (-not (Test-Path $full)) {
        Write-Output ('警告：引用的文件不存在 -> ' + $rel)
        $missing++
    }
}
Write-Output ('已检查文件引用 = ' + $checked + '，缺失 = ' + $missing)
if ($missing -gt 0) {
    Write-Output '说明：在工程目录之外运行本脚本时，Core/… 与 Drivers/… 的警告可以忽略。'
}
