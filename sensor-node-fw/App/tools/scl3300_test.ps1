<# 主机验证真实倾角补码换算、SPI错帧协议与批内滤波；不代替 L1/L5。 #>
param([string]$Gcc = 'D:\Qt\Tools\mingw1310_64\bin\gcc.exe')
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Gcc)) { throw '请用 -Gcc 指定主机 MinGW gcc.exe' }
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$testRoot = Join-Path $PSScriptRoot 'tests'
$taskTempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')
$taskWorkDir = Join-Path $taskTempRoot ('sensor-scl3300-test-' + [guid]::NewGuid().ToString('N'))
if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($taskWorkDir)) -ne $taskTempRoot) {
    throw '测试路径越出系统临时目录'
}
New-Item -ItemType Directory -Path $taskWorkDir | Out-Null
try {
    $include = @($testRoot, (Join-Path $projectRoot 'App\app'),
                 (Join-Path $projectRoot 'App\bsp'), (Join-Path $projectRoot 'App\driver'),
                 (Join-Path $projectRoot 'App\service'), (Join-Path $projectRoot 'App\component\modem'),
                 (Join-Path $projectRoot 'App\component\modem\backend'),
                 (Join-Path $projectRoot 'App\component\ble'))
    $include += Join-Path $projectRoot 'App\app_command'
    $sources = @('App\app\sensor_cycle.c', 'App\service\svc_filter.c',
                 'App\tools\tests\scl3300_test.c') | ForEach-Object { Join-Path $projectRoot $_ }
    $testExe = Join-Path $taskWorkDir 'scl3300_test.exe'
    $arguments = @('-std=c11','-O2','-Wall','-Wextra','-Werror','-static') +
                 @($include | ForEach-Object { '-I' + $_ }) + $sources + @('-o', $testExe)
    & $Gcc @arguments
    if ($LASTEXITCODE -ne 0) { throw '主机测试编译失败' }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw '倾角换算测试失败' }
} finally {
    # 删除前核对绝对路径，仅清理本次生成的测试目录。
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($taskWorkDir)) -eq $taskTempRoot) {
        Remove-Item -LiteralPath $taskWorkDir -Recurse -Force
    }
}
