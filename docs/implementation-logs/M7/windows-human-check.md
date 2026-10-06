# M7 native Windows human acceptance

**Status: passed by the user.** Physical Ctrl+C was observed, exit 130 was recorded,
the original destination bytes stayed intact, PowerShell JSON parsing passed, and
whole-file verification passed. The user reported the prompt/cancellation text;
the saved evidence was inspected at
`build/m7-human-aceb3abe0fdd4ab3822a463227eb4867/evidence.json`
(UTC `2026-10-05T17:15:31.4167440+00:00`). The agent did not simulate the human action.
Syntax and explicit array-argument binding were checked in Windows PowerShell 5.1 and
PowerShell 7.6 without executing the acceptance script.

Run the following in a native Windows Terminal PowerShell session from the repository:

```powershell
.\scripts\windows-m7-acceptance.ps1
```

If the current execution policy blocks scripts, run this reviewed local check in a
new PowerShell process with an override limited to that process:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "D:\LocalVault\scripts\windows-m7-acceptance.ps1"
```

This leaves the machine and user execution policies unchanged.

The default binary is `build\m5-junction\cmake\src\cli\Debug\localvault.exe` beneath
the repository root. Use `-Cli 'C:\path\to\localvault.exe'` for another build.
PowerShell 5 and 7 are supported. When the restore conflict prompt appears, press
**physical Ctrl+C once**, without typing a response.

The script checks repository initialization, a tiny Unicode snapshot, PowerShell
`list --json | ConvertFrom-Json`, exit 130 while terminal input is waiting, unchanged
destination bytes, and whole-file verification. It temporarily protects the
PowerShell host from cancellation so it can inspect the child result, and restores
that handler afterward. It also checks that the host observed Ctrl+C.
The wrapper temporarily configures BOM-free UTF-8 for terminal display and PowerShell
native-output decoding, then restores the original output encoding in `finally` on
success or failure.

Each run creates a unique `build\m7-human-<uuid>` fixture. It deletes no data and
preserves the fixture plus timestamped `evidence.json` on success. Report the printed
evidence path; inspect it before treating this gate as complete. A failed or interrupted
run leaves its fixture for diagnosis and does not create success evidence.

The automated mid-snapshot Windows Ctrl+Break test checks process-group interruption
and recovery when a console is available. This physical Ctrl+C check complements it by
covering the native terminal prompt and PowerShell pipeline. Automated passes or skips
do not complete this human gate.
