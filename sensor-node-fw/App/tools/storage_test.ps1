<# 主机 NOR 故障模拟：编译实际 Driver/Service，使用测试 BSP 替身；不代替 L1/L5。 #>
param([string]$Gcc = 'D:\Qt\Tools\mingw1310_64\bin\gcc.exe')
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Gcc)) { throw '请用 -Gcc 指定主机 MinGW gcc.exe（不是 ARM GCC）' }
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$testRoot = Join-Path $PSScriptRoot 'tests'
$tempRoot = [System.IO.Path]::GetFullPath($env:TEMP)
$workDir = Join-Path $tempRoot ('sensor-storage-test-' + [guid]::NewGuid().ToString('N'))
if ([System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($workDir)) -ne $tempRoot.TrimEnd('\')) {
    throw '测试目录越出系统临时目录'
}
New-Item -ItemType Directory -Path $workDir | Out-Null
try {
    $include = @($testRoot, (Join-Path $projectRoot 'App\app'), (Join-Path $projectRoot 'App\bsp'),
                 (Join-Path $projectRoot 'App\driver'), (Join-Path $projectRoot 'App\service'))
    $sources = @('App\driver\drv_storage.c','App\driver\drv_w25q32.c','App\driver\drv_w25q32_io.c',
                 'App\service\svc_storage.c','App\service\svc_param.c','App\service\svc_frame.c',
                 'App\service\svc_config_record.c','App\service\svc_config_journal.c',
                 'App\service\svc_config_scan.c','App\service\svc_config_save.c',
                 'App\app\store.c','App\app\store_params.c','App\app\store_scan.c') |
               ForEach-Object { Join-Path $projectRoot $_ }
    $testExe = Join-Path $workDir 'storage_test.exe'
    $arguments = @('-std=c11','-O2','-Wall','-Wextra','-Werror','-static') +
                 @($include | ForEach-Object { '-I' + $_ }) + $sources +
                 @((Join-Path $testRoot 'storage_test.c'), '-o', $testExe)
    & $Gcc @arguments
    if ($LASTEXITCODE -ne 0) { throw '主机测试编译失败' }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw 'NOR 故障模拟失败' }
} finally {
    # 递归清理前再次检查绝对路径，只删除本次生成的临时目录。
    if ([System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($workDir)) -eq $tempRoot.TrimEnd('\')) {
        Remove-Item -LiteralPath $workDir -Recurse -Force
    }
}
