<#
-------------------------------------------------------------------------------
 smoke_build.ps1 —— 全量编译 + 链接烟测（无需安装 Keil）

 用途：
   用 STM32CubeCLT 自带的 arm-none-eabi-gcc 编译 Core/、App/、Drivers/(HAL)、
   Qpc/(QP/C QV 内核) 与 ThirdParty/，再用一个最小的 GCC 启动文件链接出完整
   ELF，报告 Flash/RAM 占用。可以验证语法、类型与符号接线。

 注意：这是替代手段，不能取代 Keil / ARM Compiler 6 的正式编译（见 AGENTS.md 第 8 节）。

 用法（可在任意目录运行）：

   powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\smoke_build.ps1

   可选参数：
     -Root <工程目录>   含 Core/、App/、Qpc/ 的目录
     -Keep              保留中间目标文件，便于排查

 退出码：0 = 编译与链接均通过；1 = 失败
-------------------------------------------------------------------------------
#>
param(
    [string]$Root = '',
    [switch]$Keep
)

$ErrorActionPreference = 'Continue'

# --------------------------------------------------------------- 定位工程目录
if ([string]::IsNullOrWhiteSpace($Root)) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path     # <工程>\App\tools
    $Root = Split-Path -Parent (Split-Path -Parent $scriptDir)       # <工程>
}
if (-not (Test-Path (Join-Path $Root 'Core\Src'))) { throw ('未找到 Core\Src：' + $Root) }
$Root = (Resolve-Path $Root).Path

# --------------------------------------------------------------- 定位编译器
$gcc = $null
$cmd = Get-Command 'arm-none-eabi-gcc' -ErrorAction SilentlyContinue
if ($cmd) { $gcc = $cmd.Source }
if (-not $gcc) {
    $cand = @(
        'D:\ST\STM32CubeCLT_1.18.0\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe',
        'C:\ST\STM32CubeCLT_1.18.0\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe',
        'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeCLT\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe'
    )
    foreach ($c in $cand) { if (Test-Path $c) { $gcc = $c; break } }
}
if (-not $gcc) { throw '未找到 arm-none-eabi-gcc，请安装 STM32CubeCLT 或把它加入 PATH' }
$size = Join-Path (Split-Path $gcc) 'arm-none-eabi-size.exe'

$objdir = Join-Path $env:TEMP 'sensor-node-fw-smoke'
if (Test-Path $objdir) { Remove-Item $objdir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $objdir | Out-Null

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

# --------------------------------------------------------------- 头文件搜索路径
$inc = @(
    (Join-Path $Root 'Core\Inc'),
    (Join-Path $Root 'Drivers\STM32F1xx_HAL_Driver\Inc'),
    (Join-Path $Root 'Drivers\STM32F1xx_HAL_Driver\Inc\Legacy'),
    (Join-Path $Root 'Drivers\CMSIS\Device\ST\STM32F1xx\Include'),
    (Join-Path $Root 'Drivers\CMSIS\Include')
)
foreach ($layer in @('App\app', 'App\app_command', 'App\service', 'App\driver', 'App\bsp', 'App\component')) {
    $d = Join-Path $Root $layer
    if (Test-HasHeader $d) {
        $inc += $d
        $inc += @(Get-ChildItem $d -Recurse -Filter *.h -File |
            Select-Object -ExpandProperty DirectoryName)
    }
}
if (Test-HasHeader (Join-Path $Root 'Qpc\platform\inc')) { $inc += (Join-Path $Root 'Qpc\platform\inc') }
foreach ($p in @(Get-ChildItem (Join-Path $Root 'Qpc\port') -Directory -ErrorAction SilentlyContinue)) {
    if (Test-HasHeader (Join-Path $p.FullName 'inc')) { $inc += (Join-Path $p.FullName 'inc') }
}
foreach ($lib in @(Get-ChildItem (Join-Path $Root 'ThirdParty') -Directory -ErrorAction SilentlyContinue)) {
    foreach ($sub in @($lib.FullName, (Join-Path $lib.FullName 'inc'))) {
        if (Test-HasHeader $sub) { $inc += $sub }
    }
}
$inc = @($inc | Select-Object -Unique)
$common = @('-mcpu=cortex-m3', '-mthumb', '-std=c11', '-O1', '-Wall',
    '-DUSE_HAL_DRIVER', '-DSTM32F103xE', '-DQV_PRJ') + ($inc | ForEach-Object { '-I' + $_ })

# -------------------------------------------------------------------- 源文件清单
$src = @()
$src += @(Get-ChildItem (Join-Path $Root 'Core\Src') -Filter *.c -File | Select-Object -ExpandProperty FullName)
foreach ($layer in @('App\app', 'App\app_command', 'App\service', 'App\driver', 'App\bsp', 'App\component')) {
    $src += @(Get-CFiles (Join-Path $Root $layer))
}
$src += @(Get-ChildItem (Join-Path $Root 'Drivers\STM32F1xx_HAL_Driver\Src') -Filter *.c -File |
    Where-Object { $_.Name -notmatch '_template\.c$' } | Select-Object -ExpandProperty FullName)
$src += @(Get-ChildItem (Join-Path $Root 'Qpc\platform\src') -Filter *.c -File |
    Where-Object { $_.Name -notin @('qk.c', 'qxk.c', 'qxk_mutex.c', 'qxk_sema.c', 'qxk_xthr.c') } |
    Select-Object -ExpandProperty FullName)
$src += @(Get-ChildItem (Join-Path $Root 'Qpc\port') -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Get-CFiles $_.FullName })
foreach ($lib in @(Get-ChildItem (Join-Path $Root 'ThirdParty') -Directory -ErrorAction SilentlyContinue)) {
    $src += @(Get-CFiles $lib.FullName)
}

# 最小 GCC 启动文件（仅烟测用；Keil 工程用的是 ST 的 armasm 启动文件）
$startupPath = Join-Path $objdir 'smoke_start.c'
$startup = @'
/* 最小 GCC 启动代码 + 向量表（仅供本烟测使用） */
extern int main(void);
void _start(void) { (void)main(); for (;;) { } }
void Default_Handler(void) { for (;;) { } }
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))0x2000C000U,  /* 初始栈指针（48K RAM 顶部） */
    _start,
    Default_Handler,
    Default_Handler,
};
'@
[System.IO.File]::WriteAllText($startupPath, $startup, (New-Object System.Text.UTF8Encoding($false)))
$src += $startupPath

Write-Output ('工程目录：' + $Root)
Write-Output ('编译器　：' + $gcc)
Write-Output ('文件数量：' + $src.Count)
Write-Output ''

# ---------------------------------------------------------------------- 编译
$errCount = 0
$warnCount = 0
$objs = @()
$log = @()
foreach ($f in $src) {
    $obj = Join-Path $objdir (([System.IO.Path]::GetFileNameWithoutExtension($f)) + '.o')
    $out = (& $gcc @common -c $f -o $obj 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        $errCount++
        $log += ('--- 编译失败：' + $f)
        $log += ($out -split "`n" | Where-Object { $_ -match 'error:|Error' } | Select-Object -First 8)
    }
    else {
        $objs += $obj
        $w = @($out -split "`n" | Where-Object { $_ -match 'warning:' })
        if ($w.Count -gt 0) {
            $warnCount += $w.Count
            $log += ('--- 警告：' + [System.IO.Path]::GetFileName($f))
            $log += ($w | Select-Object -First 5)
        }
    }
}
Write-Output ('编译：errors = ' + $errCount + '，warnings = ' + $warnCount)
if ($log.Count -gt 0) { $log | Select-Object -First 50 }

# ---------------------------------------------------------------------- 链接
if ($errCount -eq 0) {
    $elf = Join-Path $objdir 'smoke.elf'
    $map = Join-Path $objdir 'smoke.map'
    $linkArgs = @(
        '-Wl,--defsym=_estack=0x2000C000',
        '-nostartfiles',
        '--specs=nosys.specs',
        '-Wl,-Ttext=0x08000000',
        ('-Wl,-Map=' + $map)
    )
    $linkOut = (& $gcc @common @linkArgs $objs -o $elf 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        Write-Output '链接：失败'
        $linkOut -split "`n" | Select-Object -First 25
        exit 1
    }
    Write-Output '链接：通过（所有符号都已解析）'
    Write-Output ''
    Write-Output '镜像占用（text / data / bss）：'
    & $size $elf
    Write-Output ''
    Write-Output '目标芯片：256 KB Flash / 48 KB RAM（STM32F103RCT6）'
}

if (-not $Keep) { Remove-Item $objdir -Recurse -Force -ErrorAction SilentlyContinue }
if ($errCount -ne 0) { exit 1 }
exit 0
