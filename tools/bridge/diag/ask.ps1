# 直接与 sai2_agent 命名管道对话（绕过 CLI 的引号问题）
param(
    [Parameter(Mandatory = $true)][string]$Cmd,
    [int]$TimeoutMs = 4000
)

$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
    '.', 'sai2agent', [System.IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect($TimeoutMs)
} catch {
    Write-Output "CONNECT_FAIL: $($_.Exception.Message)"
    exit 2
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($pipe, $utf8)
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($pipe, $utf8)

$writer.WriteLine($Cmd)
$line = $reader.ReadLine()

$pipe.Dispose()
Write-Output $line
