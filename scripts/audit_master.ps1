[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [string]$SshHost = 'phuong',
    [string[]]$StopSystemService = @(),
    [string[]]$StopContainer = @(),
    [string[]]$StopPm2Application = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$protectedServices = @(
    'containerd', 'cron', 'dbus', 'docker', 'grafana-server', 'nginx',
    'node_exporter', 'prometheus', 'redis-server', 'rsyslog', 'ssh',
    'systemd-journald', 'systemd-logind', 'systemd-networkd',
    'systemd-resolved', 'systemd-timesyncd', 'systemd-udevd'
)
$protectedContainers = @('mysql_db', 'kokoro-fastapi-cpu-kokoro-tts-1', 'qas-master', 'qas-prometheus', 'qas-grafana')

Write-Host '--- Running services ---'
& ssh -o BatchMode=yes $SshHost systemctl --no-pager --plain --state=running --type=service
Write-Host '--- Containers ---'
& ssh -o BatchMode=yes $SshHost docker ps --format '{{.Names}} {{.Image}} {{.Status}}'
Write-Host '--- Largest processes ---'
& ssh -o BatchMode=yes $SshHost ps -eo pid,comm,%cpu,%mem,rss,args --sort=-rss

foreach ($service in $StopSystemService) {
    if ($service -notmatch '^[a-zA-Z0-9@_.-]+$') {
        throw "Invalid systemd service: $service"
    }
    $canonical = $service -replace '\.service$', ''
    if ($protectedServices -contains $canonical) {
        throw "Protected service cannot be stopped: $service"
    }
    if ($PSCmdlet.ShouldProcess("$SshHost/$service", 'Stop systemd service')) {
        & ssh -o BatchMode=yes $SshHost systemctl stop "$canonical.service"
    }
}

foreach ($container in $StopContainer) {
    if ($container -notmatch '^[a-zA-Z0-9_.-]+$') {
        throw "Invalid container: $container"
    }
    if ($protectedContainers -contains $container) {
        throw "Protected container cannot be stopped: $container"
    }
    if ($PSCmdlet.ShouldProcess("$SshHost/$container", 'Stop Docker container')) {
        & ssh -o BatchMode=yes $SshHost docker stop $container
    }
}

foreach ($application in $StopPm2Application) {
    if ($application -notmatch '^[a-zA-Z0-9 _.-]+$') {
        throw "Invalid PM2 application: $application"
    }
    if ($PSCmdlet.ShouldProcess("$SshHost/$application", 'Stop PM2 application')) {
        & ssh -o BatchMode=yes $SshHost pm2 stop $application
    }
}
