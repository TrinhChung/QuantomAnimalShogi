[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$TaskName = 'QuantumAnimalShogiWorker',

    [ValidateRange(1, 120)]
    [int]$IdleMinutes = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not [Environment]::GetEnvironmentVariable('QAS_CLUSTER_TOKEN', 'User')) {
    throw 'Set the user-level QAS_CLUSTER_TOKEN before registering the task'
}

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$workerBatch = Join-Path $repositoryRoot 'scripts\cluster_worker_via_ssh.bat'
$escapedCommand = [System.Security.SecurityElement]::Escape("/d /c `"$workerBatch`"")
$escapedUser = [System.Security.SecurityElement]::Escape(
    [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
)
$userSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$escapedSid = [System.Security.SecurityElement]::Escape($userSid)
$startBoundary = [DateTime]::UtcNow.AddMinutes(1).ToString('s') + 'Z'
$xml = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Runs the Quantum Animal Shogi worker only while this PC is idle.</Description>
  </RegistrationInfo>
  <Triggers>
    <IdleTrigger><Enabled>true</Enabled></IdleTrigger>
    <LogonTrigger><StartBoundary>$startBoundary</StartBoundary><Enabled>true</Enabled><UserId>$escapedUser</UserId></LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author"><UserId>$escapedSid</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>true</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>true</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>true</RunOnlyIfNetworkAvailable>
    <IdleSettings><Duration>PT${IdleMinutes}M</Duration><WaitTimeout>PT24H</WaitTimeout><StopOnIdleEnd>true</StopOnIdleEnd><RestartOnIdle>true</RestartOnIdle></IdleSettings>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author"><Exec><Command>cmd.exe</Command><Arguments>$escapedCommand</Arguments><WorkingDirectory>$repositoryRoot</WorkingDirectory></Exec></Actions>
</Task>
"@

if ($PSCmdlet.ShouldProcess($TaskName, 'Register idle cluster worker task')) {
    Register-ScheduledTask -TaskName $TaskName -Xml $xml -Force | Out-Null
    Write-Host "Registered $TaskName; it starts after $IdleMinutes idle minutes and stops when activity resumes."
}
