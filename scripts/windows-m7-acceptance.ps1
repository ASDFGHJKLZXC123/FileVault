param([string]$Cli)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Cli) { $Cli = Join-Path $repoRoot 'build\m5-junction\cmake\src\cli\Debug\localvault.exe' }
if ($env:OS -ne 'Windows_NT' -or -not (Test-Path -LiteralPath $Cli -PathType Leaf)) {
    throw 'Run in a native Windows terminal with the built CLI; use -Cli to specify another binary.'
}
if (-not ('LocalVaultM7HumanConsole' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class LocalVaultM7HumanConsole {
    public delegate bool Handler(uint signal);
    public static int CtrlCCount;
    private static readonly Handler handler = KeepHost;
    private static bool KeepHost(uint signal) {
        if (signal == 0) Interlocked.Increment(ref CtrlCCount);
        return signal == 0 || signal == 1;
    }
    [DllImport("kernel32.dll")] private static extern bool SetConsoleCtrlHandler(Handler h, bool add);
    [DllImport("kernel32.dll")] public static extern uint GetConsoleCP();
    public static bool Shield(bool add) { return SetConsoleCtrlHandler(handler, add); }
}
'@
}
if ([LocalVaultM7HumanConsole]::GetConsoleCP() -eq 0) { throw 'A native console is required; use Windows Terminal.' }
function Invoke-Checked([string[]]$Arguments) {
    $output = & $Cli @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CLI failed with exit $LASTEXITCODE : $Arguments" }
    $output | ConvertFrom-Json
}
$previousOutputEncoding = [Console]::OutputEncoding
$utf8 = New-Object Text.UTF8Encoding($false)
try {
    [Console]::OutputEncoding = $utf8
    $fixture = Join-Path $repoRoot ('build\m7-human-' + [guid]::NewGuid().ToString('N'))
    $source = Join-Path $fixture 'source'
    $vault = Join-Path $fixture 'vault'
    $destination = Join-Path $fixture 'destination'
    [void][IO.Directory]::CreateDirectory($source)
    [void][IO.Directory]::CreateDirectory($destination)
    $name = ([string][char]0x6D4B) + [char]0x8BD5 + '.txt'
    [IO.File]::WriteAllText((Join-Path $source $name), ('saved ' + [char]0x4E16 + [char]0x754C), $utf8)
    $null = Invoke-Checked -Arguments @('init', $vault, '--json')
    $snapshot = Invoke-Checked -Arguments @('snapshot', $source, '--repo', $vault, '--json', '--quiet')
    $id = $snapshot.result.snapshot_id
    $listing = & $Cli list --repo $vault --json | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $listing.schema_version -ne 1 -or $listing.result.snapshots[0].id -ne $id) {
        throw 'PowerShell list/JSON pipeline check failed.'
    }
    $original = [Text.Encoding]::UTF8.GetBytes('existing destination must remain')
    $target = Join-Path $destination $name
    [IO.File]::WriteAllBytes($target, $original)
    Write-Host 'At the upcoming restore prompt, press physical Ctrl+C ONCE. Do not type an answer.'
    $before = [LocalVaultM7HumanConsole]::CtrlCCount
    $child = $null
    if (-not [LocalVaultM7HumanConsole]::Shield($true)) { throw 'Could not protect the PowerShell host.' }
    try {
        $arguments = 'restore {0} "{1}" --repo "{2}" --output "{3}" --overwrite prompt --quiet' -f $id, $name, $vault, $destination
        $child = Start-Process -FilePath $Cli -ArgumentList $arguments -NoNewWindow -PassThru -Wait
        $cancelCode = $child.ExitCode
    } finally {
        if ($child) { $child.Dispose() }
        [void][LocalVaultM7HumanConsole]::Shield($false)
    }
    if ($cancelCode -ne 130 -or [LocalVaultM7HumanConsole]::CtrlCCount -le $before) {
        throw 'Physical Ctrl+C acceptance failed: expected an observed Ctrl+C and CLI exit 130.'
    }
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($target)) -cne [Convert]::ToBase64String($original)) {
        throw 'Cancellation changed the original destination bytes.'
    }
    $verified = Invoke-Checked -Arguments @('verify', '--repo', $vault, '--files', '--json', '--quiet')
    if (-not $verified.result.ok) { throw 'Whole-file verification failed.' }
    $evidence = Join-Path $fixture 'evidence.json'
    $record = [ordered]@{
        status = 'passed by human'; timestamp_utc = [DateTimeOffset]::UtcNow.ToString('o');
        cli = (Resolve-Path -LiteralPath $Cli).Path; fixture = $fixture; snapshot_id = $id;
        physical_ctrl_c_observed = $true; exit_code = $cancelCode; destination_unchanged = $true;
        powershell_json_pipeline_ok = $true; whole_file_verification_ok = $true
    }
    [IO.File]::WriteAllText($evidence, ($record | ConvertTo-Json), $utf8)
    Write-Host "Human acceptance passed at $($record.timestamp_utc). Evidence: $evidence"
    Write-Host "Fixtures preserved: $fixture"
} finally {
    [Console]::OutputEncoding = $previousOutputEncoding
}
