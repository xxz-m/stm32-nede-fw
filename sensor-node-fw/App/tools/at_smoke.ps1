<#
-------------------------------------------------------------------------------
 at_smoke.ps1 —— 调试口 AT 命令 + 参数存储的上板验证（L5 硬件在环）

 用途：
   连上调试串口（USART1，115200 8N1），按固定顺序把 AT 命令发一遍，检查
   启动横幅、每条响应，以及**参数是否真的落了盘**。默认全程只读，
   只有显式给出 -WriteTest / -WriteSn 才会写 EEPROM。

 命令形态（标准 AT，见 App/service/svc_at.h 与 docs_sensor/06-协议格式.md）：
   AT                      握手                      -> OK
   AT&F                    恢复编译期默认值并落盘      -> OK
   AT+CFG?                 存储运行状态               -> +CFG:rdy,present,loaded,dirty,ver,seq,ok,fail
   AT+I2C?                 扫描 I2C 总线              -> OK，随后 +I2C:<地址>… +I2C:END
   AT+<参数名>?            读参数                     -> +<参数名>:"<值>"
   AT+<参数名>="<值>"      写参数                     -> OK
   AT+<参数名>=?           查最大长度                 -> +<参数名>:(<最大长度>)
   参数名：SN NAME PIN UUID DEVNAME MODEL BATCH；大小写不敏感，每个命令都要 CR/LF 结尾。

 为什么不只用串口助手：
   这一轮最贵的两个故障——「宏配了但没生效」与「回了 OK 却没落盘」——靠肉眼看
   串口滚屏都很难发现。脚本把这两件事变成明确的 PASS/FAIL。

 用法（在任意目录均可运行）：

   # 看有哪些串口
   powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\at_smoke.ps1 -ListPorts

   # 只读检查（推荐先跑这个）
   powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\at_smoke.ps1 -Port COM5

   # 追加无损写入测试：把当前 SN 原值再写一遍，验证「编码→页写→回读校验」整条路
   powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\at_smoke.ps1 -Port COM5 -WriteTest

   # 有损：写入指定 SN（脚本会打印原值，便于还原）
   powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\at_smoke.ps1 -Port COM5 -WriteSn VWA2608A0020

 退出码：0 = 全部检查通过；1 = 有检查失败或无法打开串口

 注意：
   - 需要真实硬件与串口，**无法在 CI 里跑**；它只做「发命令 + 比对响应」，
     不替代人眼确认横幅里的信息（脚本会把该看的行打出来）。
   - 参数存储的「断电保持」必须人工断电复测，脚本只能在最后提醒你；
     见 docs_sensor/06-协议格式.md#怎么验证。
   - `pin` 是凭据：脚本只校验响应格式，**不打印其值**（AGENTS.md 4.2 的凭据纪律）。
   - 只用 PowerShell 5.1 就有的语法（不用三目运算符、不用 ??），Windows 自带即可跑。
-------------------------------------------------------------------------------
#>
param(
    [string]$Port = '',
    [int]$Baud = 115200,
    [switch]$ListPorts,
    [switch]$SkipBoot,
    [switch]$WriteTest,
    [string]$WriteSn = '',
    [int]$TimeoutMs = 1500
)

$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------------ 只列串口
if ($ListPorts) {
    Write-Output '本机串口：'
    $names = @([System.IO.Ports.SerialPort]::GetPortNames())
    if ($names.Count -eq 0) { Write-Output '  (没找到任何串口——USB-TTL 驱动装了吗？)' }
    foreach ($n in $names) { Write-Output ('  ' + $n) }
    exit 0
}

# ------------------------------------------------------------------ 选串口
if ([string]::IsNullOrWhiteSpace($Port)) {
    $names = @([System.IO.Ports.SerialPort]::GetPortNames())
    if ($names.Count -eq 1) {
        $Port = $names[0]
        Write-Host ('自动选中唯一的串口：' + $Port) -ForegroundColor DarkGray
    }
    else {
        Write-Host ('请用 -Port 指定串口（当前检测到 ' + $names.Count + ' 个）：') -ForegroundColor Yellow
        foreach ($n in $names) { Write-Host ('  ' + $n) }
        exit 1
    }
}

# ------------------------------------------------------------------ 状态与工具
$script:sp = $null
$script:rx = New-Object System.Text.StringBuilder
$script:pass = 0
$script:fail = 0
$script:warn = 0

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail = '')
    $tail = ''
    if (-not [string]::IsNullOrWhiteSpace($Detail)) { $tail = '   <- ' + $Detail }
    if ($Ok) {
        $script:pass++
        Write-Host ('  [PASS] ' + $Name + $tail) -ForegroundColor Green
    }
    else {
        $script:fail++
        Write-Host ('  [FAIL] ' + $Name + $tail) -ForegroundColor Red
    }
}

function Warn {
    param([string]$Text)
    $script:warn++
    Write-Host ('  [WARN] ' + $Text) -ForegroundColor Yellow
}

function Section {
    param([string]$Title)
    Write-Host ''
    Write-Host ('=== ' + $Title + ' ===') -ForegroundColor Cyan
}

# 串口收发 -------------------------------------------------------------------
function Read-Available {
    <# 把当前可读字节读进累积缓冲（固件输出按设计全是 ASCII）。 #>
    if ($null -eq $script:sp) { return }
    $n = $script:sp.BytesToRead
    if ($n -le 0) { return }
    $buf = New-Object byte[] $n
    $got = $script:sp.Read($buf, 0, $n)
    [void]$script:rx.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $got))
}

function Pump-Lines {
    <# 取出缓冲里所有完整行（已去行尾）；不完整的部分留在缓冲里等下次。 #>
    $lines = New-Object System.Collections.ArrayList
    while ($true) {
        $text = $script:rx.ToString()
        $idx = $text.IndexOf("`n")
        if ($idx -lt 0) { break }
        $line = $text.Substring(0, $idx).TrimEnd("`r")
        [void]$script:rx.Remove(0, $idx + 1)
        if ($line.Length -gt 0) { [void]$lines.Add($line) }
    }
    return $lines
}

function Wait-Match {
    <# 等到某一行匹配正则；超时返回 $null。不匹配的行直接丢弃（避免被周期日志干扰）。 #>
    param([string]$Pattern, [int]$Ms = $TimeoutMs)
    $deadline = (Get-Date).AddMilliseconds($Ms)
    while ((Get-Date) -lt $deadline) {
        Read-Available
        foreach ($line in (Pump-Lines)) {
            if ($line -match $Pattern) { return $line }
        }
        Start-Sleep -Milliseconds 20
    }
    return $null
}

function Collect-Lines {
    <# 在给定时限内收集所有行（抓启动横幅用），返回 ArrayList。 #>
    param([int]$Ms = 1000)
    $all = New-Object System.Collections.ArrayList
    $deadline = (Get-Date).AddMilliseconds($Ms)
    while ((Get-Date) -lt $deadline) {
        Read-Available
        foreach ($line in (Pump-Lines)) { [void]$all.Add($line) }
        Start-Sleep -Milliseconds 50
    }
    return $all
}

function Send-At {
    param([string]$Command)
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Command + "`r`n")
    $script:sp.Write($bytes, 0, $bytes.Length)
    Write-Host ('  -> ' + $Command) -ForegroundColor DarkGray
}

function Ask {
    <# 发一条命令并等到匹配的响应行；超时返回 $null。 #>
    param([string]$Command, [string]$Pattern, [int]$Ms = $TimeoutMs)
    Send-At $Command
    return (Wait-Match -Pattern $Pattern -Ms $Ms)
}

function Get-QuotedValue {
    <# 从 `+SN:"abc"` 这类响应里取出 abc（去掉字段名与成对引号）。 #>
    param([string]$Line)
    $i = $Line.IndexOf(':')
    if ($i -lt 0) { return '' }
    $v = $Line.Substring($i + 1).Trim()
    if (($v.Length -ge 2) -and $v.StartsWith('"') -and $v.EndsWith('"')) {
        $v = $v.Substring(1, $v.Length - 2)
    }
    return $v
}

# ------------------------------------------------------------------ 打开串口
try {
    $script:sp = New-Object System.IO.Ports.SerialPort $Port, $Baud,
        ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $script:sp.ReadTimeout = 200
    $script:sp.WriteTimeout = 1000
    $script:sp.Open()
}
catch {
    Write-Host ('打不开 ' + $Port + '：' + $_.Exception.Message) -ForegroundColor Red
    Write-Host '（串口被别的程序占用？先关掉串口助手 / Keil 的串口窗口）' -ForegroundColor Yellow
    if ($null -ne $script:sp) { $script:sp.Dispose() }
    exit 1
}

Write-Host ('已打开 ' + $Port + ' @ ' + $Baud + ' 8N1') -ForegroundColor DarkGray

# ------------------------------------------------------------------ 主流程
try {
    # ---------------------------------------------------- 1) 启动横幅
    if (-not $SkipBoot) {
        Section '1) 启动横幅（需要你按一下复位键）'
        Write-Host '请按一下板子的复位键（或重新上电），最多等 8 秒…' -ForegroundColor Yellow

        $banner = New-Object System.Collections.ArrayList
        $seenBootOk = $false
        $deadline = (Get-Date).AddSeconds(8)
        while ((Get-Date) -lt $deadline) {
            Read-Available
            foreach ($line in (Pump-Lines)) {
                [void]$banner.Add($line)
                Write-Host ('     ' + $line) -ForegroundColor DarkGray
                if ($line -match 'boot ok') { $seenBootOk = $true }
            }
            if ($seenBootOk) { break }
            Start-Sleep -Milliseconds 50
        }

        if ($banner.Count -eq 0) {
            Warn '没抓到任何输出——跳过横幅检查（确认串口/波特率/PA9-PA10 接线，或加 -SkipBoot）'
        }
        else {
            if (-not $seenBootOk) {
                Warn '抓到了输出但没有 "boot ok"：复位可能按晚了；下面按「抓到什么查什么」继续'
            }
            $joined = $banner -join "`n"

            Check '启动横幅（==== sensor-node-fw ====）' ($joined -match '==== sensor-node-fw ====') ''
            Check 'I2C 初始化行（i2c : I2C1 PB6/PB7 …）' ($joined -match 'i2c\s*:\s*I2C1') ''
            Check 'AO 列表含 store p6' ($joined -match 'actors\s*:.*store p6') ''
            Check '启动完成（boot ok）' ($joined -match 'boot ok') ''
            Check 'AT 命令清单已打印（at console ready）' ($joined -match 'at console ready') ''

            # 下面两条是 #25「配置了但没生效」修复的验收点：读的是外设里的实际值。
            $baudLine = $null
            $baudVal = ''
            foreach ($ln in $banner) {
                if ($ln -match 'modem:\s*(\S+)\s+on\s+USART(\d+)\s+(\d+)') {
                    $baudLine = $ln
                    $baudVal = $Matches[3]
                    break
                }
            }
            if ($null -ne $baudLine) {
                Check ('4G 实际生效波特率（' + $baudVal + '）') ($baudVal -eq '9600') $baudLine
                if ($baudVal -ne '9600') {
                    Warn 'MN316 档应为 9600：先确认模组真实速率，再改 APP_CFG_MODEM_UART_BAUD（只改一行）'
                }
            }
            else {
                Warn '横幅里没有 modem: 那行（APP_CFG_MODEM_ENABLE 关了？）'
            }

            $bleLine = $null
            $bleBaud = ''
            foreach ($ln in $banner) {
                if ($ln -match 'ble:\s*VG6328\s+on\s+USART(\d+)\s+(\d+)') {
                    $bleLine = $ln
                    $bleBaud = $Matches[2]
                    break
                }
            }
            if ($null -ne $bleLine) {
                Check ('BLE 实际生效波特率（' + $bleBaud + '）') ($bleBaud -eq '115200') $bleLine
            }

            if ($joined -match 'store:\s*eeprom at 0x50 ready') {
                Check 'EEPROM 应答（eeprom at 0x50 ready）' $true ''
            }
            elseif ($joined -match 'store:\s*no eeprom') {
                Check 'EEPROM 应答（eeprom at 0x50 ready）' $false '报的是 no eeprom：查 PB6/PB7 接线与上拉、A1/A2 接法'
            }
            else {
                Warn '横幅里没有 store: 的探测结果行'
            }
        }
    }

    # ---------------------------------------------------- 2) 只读命令
    Section '2) 只读命令（不碰 EEPROM）'
    # Mask = 该响应含凭据，只校验格式、不打印值（AGENTS.md 4.2 凭据纪律）
    $reads = @(
        @{ Cmd = 'AT';              Pat = '^OK$';            Mask = $false },
        @{ Cmd = 'AT+SN?';          Pat = '^\+SN:"';         Mask = $false },
        @{ Cmd = 'AT+NAME?';        Pat = '^\+NAME:"';       Mask = $false },
        @{ Cmd = 'AT+PIN?';         Pat = '^\+PIN:"';        Mask = $true  },
        @{ Cmd = 'AT+UUID?';        Pat = '^\+UUID:"';       Mask = $false },
        @{ Cmd = 'AT+DEVNAME?';     Pat = '^\+DEVNAME:"';    Mask = $false },
        @{ Cmd = 'AT+MODEL?';       Pat = '^\+MODEL:"';      Mask = $false },
        @{ Cmd = 'AT+BATCH?';       Pat = '^\+BATCH:"';      Mask = $false },
        @{ Cmd = 'AT+CFG?';         Pat = '^\+CFG:\d';       Mask = $false }
    )
    foreach ($r in $reads) {
        $line = Ask -Command $r.Cmd -Pattern $r.Pat
        if ($null -eq $line) {
            Check ($r.Cmd + ' -> ' + $r.Pat) $false '超时，没有任何响应'
        }
        elseif ($r.Mask) {
            Check ($r.Cmd + ' -> ' + $r.Pat) $true ('值已隐藏，长度 ' + $line.Length)
        }
        else {
            Check ($r.Cmd + ' -> ' + $r.Pat) $true $line
        }
    }

    # 参数名大小写不敏感（AT 惯例）：小写也要能查到
    $line = Ask -Command 'AT+sn?' -Pattern '^\+SN:"'
    Check 'AT+sn? 大小写不敏感' ($null -ne $line) $line

    # 测试形式：报告可写最大长度（SN 容量 24，故为 23）
    $line = Ask -Command 'AT+SN=?' -Pattern '^\+SN:\(\d+\)$'
    Check 'AT+SN=? 报告最大长度' ($null -ne $line) $line
    if (($null -ne $line) -and ($line -notmatch '^\+SN:\(23\)$')) {
        Warn 'AT+SN=? 不是 (23)：确认 SVC_PARAM_BLE_SN_CAP 是否被改过'
    }

    # AT+CFG? 是位置式字段：rdy,present,loaded,dirty,ver,seq,ok,fail
    $cfgLine = Ask -Command 'AT+CFG?' -Pattern '^\+CFG:'
    if ($null -ne $cfgLine) {
        $fields = $cfgLine.Substring(5).Split(',')
        if ($fields.Count -eq 8) {
            Check 'AT+CFG? 字段数为 8（位置式）' $true ($cfgLine + '  ver=' + $fields[4] + ' seq=' + $fields[5])
        }
        else {
            Check 'AT+CFG? 字段数为 8（位置式）' $false ('实际 ' + $fields.Count + ' 个：' + $cfgLine)
        }
    }

    # 错误路径
    $line = Ask -Command 'NOTACMD' -Pattern '^(ERROR|\+CME ERROR:)'
    Check '未识别命令（缺 AT 前缀）回 ERROR' ($null -ne $line) $line

    $line = Ask -Command 'AT+NOSUCH?' -Pattern '^\+CME ERROR:1$'
    Check '不存在的参数名回 +CME ERROR:1' ($null -ne $line) $line

    $line = Ask -Command 'AT+SN=' -Pattern '^(ERROR|\+CME ERROR:)'
    Check '空值回 ERROR / +CME ERROR' ($null -ne $line) $line

    # ---------------------------------------------------- 3) I2C 扫描（只读）
    Section '3) I2C 扫描（只读）'
    $line = Ask -Command 'AT+I2C?' -Pattern '^OK$'
    Check 'AT+I2C? 被受理（回 OK）' ($null -ne $line) $line

    $scan = New-Object System.Collections.ArrayList
    $endLine = $null
    $deadline = (Get-Date).AddMilliseconds(5000)
    while ((Get-Date) -lt $deadline) {
        Read-Available
        foreach ($l in (Pump-Lines)) {
            if ($l -match '^\+I2C:END$') { $endLine = $l; break }
            if ($l -match '^\+I2C:[0-9A-F]{2}$') { [void]$scan.Add($l) }
        }
        if ($null -ne $endLine) { break }
        Start-Sleep -Milliseconds 20
    }
    Check '扫描收尾（+I2C:END）' ($null -ne $endLine) $endLine
    $addrs = ($scan -join ' ')
    Check ('扫描发现 EEPROM 0x50（共 ' + $scan.Count + ' 个：' + $addrs + '）') `
          ($scan -contains '+I2C:50') $addrs

    # ---------------------------------------------------- 4) 写入测试
    if ($WriteTest -or -not [string]::IsNullOrWhiteSpace($WriteSn)) {
        Section '4) 写入测试（会动 EEPROM）'

        $orig = ''
        $cur = Ask -Command 'AT+SN?' -Pattern '^\+SN:"'
        if ($null -ne $cur) { $orig = Get-QuotedValue $cur }

        if ([string]::IsNullOrWhiteSpace($orig)) {
            Warn '读不到当前 SN，跳过写入测试'
        }
        else {
            $target = $orig
            if (-not [string]::IsNullOrWhiteSpace($WriteSn)) { $target = $WriteSn }

            $cmd = 'AT+SN="' + $target + '"'
            $line = Ask -Command $cmd -Pattern '^OK$'
            Check ($cmd + ' 被受理（回 OK）') ($null -ne $line) $line

            # 关键判读：OK 只代表「已受理」，落盘证据是下面这一行
            $saved = Wait-Match -Pattern 'store:\s*saved' -Ms 4000
            Check '落盘成功（store: saved …，含回读校验）' ($null -ne $saved) $saved

            $line = Ask -Command 'AT+SN?' -Pattern '^\+SN:"'
            if ($null -ne $line) {
                $now = Get-QuotedValue $line
                Check ('影子里的新值（' + $now + '）') ($now -eq $target) $line
            }

            $cfgLine = Ask -Command 'AT+CFG?' -Pattern '^\+CFG:'
            if ($null -ne $cfgLine) {
                $fields = $cfgLine.Substring(5).Split(',')
                if ($fields.Count -eq 8) {
                    Check 'AT+CFG? 显示 loaded=1、dirty=0' `
                          (($fields[2] -eq '1') -and ($fields[3] -eq '0')) $cfgLine
                }
            }

            if (-not [string]::IsNullOrWhiteSpace($WriteSn)) {
                Write-Host ''
                Write-Host ('要还原成原值，请再执行一次： -WriteSn ' + $orig) -ForegroundColor Yellow
                Write-Host '（或发 AT&F 把全部参数恢复成编译期默认值）' -ForegroundColor Yellow
            }
        }
    }
    else {
        Section '4) 写入测试'
        Write-Host '  已跳过（默认只读）。要测写入请加 -WriteTest 或 -WriteSn <值>。' -ForegroundColor DarkGray
    }
}
catch {
    Write-Host ''
    Write-Host ('出错了：' + $_.Exception.Message) -ForegroundColor Red
    $script:fail++
}
finally {
    if ($null -ne $script:sp) {
        try { if ($script:sp.IsOpen) { $script:sp.Close() } } catch { }
        $script:sp.Dispose()
    }
}

# ------------------------------------------------------------------ 结论
Section '结果'
Write-Host ('  通过 ' + $script:pass + ' 项，失败 ' + $script:fail + ' 项，警告 ' + $script:warn + ' 项')
Write-Host ''
Write-Host '人工必做的一步：断电保持' -ForegroundColor Cyan
Write-Host '  1) 若刚做过写入测试，先确认上面出现了 store: saved；'
Write-Host '  2) 拔掉板子电源等 3 秒再上电；'
Write-Host '  3) 重跑一次本脚本（不加 -WriteTest）：AT+SN? 应等于刚才写入的值，'
Write-Host '     且 AT+CFG? 的 loaded 字段=1、ver 不回退（说明读的是 EEPROM、不是默认值）。'
Write-Host '  4) 要恢复出厂默认值：发 AT&F'

if ($script:fail -gt 0) { exit 1 }
exit 0
