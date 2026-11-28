# Admission for heavy agent commands (regen/converters through validation-lock -Write, tools/check.ps1),
# dot-sourced by both. Agents themselves barely use CPU; their regen, builds and checks arrive in bursts
# (97% of 12 threads at peaks on 2026-10-26), so with 9 agents heavy work waits for headroom and runs
# below normal priority, keeping codex-lb and the agents responsive. Native queries only: WMI is denied
# in the Codex sandbox. Tests may predefine the HK3DS.ValidationMemory / HK3DS.ValidationCpu types.
#   HK3DS_WRITE_MIN_FREE_GB      strict RAM floor (default 4)
#   HK3DS_HEAVY_MAX_CPU          CPU busy percent above which new heavy work waits (default 85)
#   HK3DS_HEAVY_MAX_CPU_WAIT_MINUTES  then it runs anyway, so it never starves (default 15)
#   HK3DS_HEAVY_PRIORITY         priority class for the command and its children (default BelowNormal)
# Windows asks kernel32; Linux reads /proc (issue #1: GlobalMemoryStatusEx is not on Linux, so every regen
# failed with "Value cannot be null (path1)"). Elsewhere memory and CPU count as free: never block a build.
if (-not ('HK3DS.ValidationMemory' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
namespace HK3DS {
    public static class ValidationMemory {
        [StructLayout(LayoutKind.Sequential)]
        private struct MemoryStatus {
            public uint Length, Load;
            public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile;
            public ulong TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
        private static MemoryStatus Windows() {
            var status = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
            if (!GlobalMemoryStatusEx(ref status))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return status;
        }
        // /proc/meminfo field in bytes; null when the file or field is missing.
        public static ulong? MemInfo(string field) {
            if (!File.Exists("/proc/meminfo")) return null;
            foreach (var line in File.ReadAllLines("/proc/meminfo")) {
                if (!line.StartsWith(field + ":")) continue;
                var parts = line.Substring(field.Length + 1).Trim().Split(' ');
                ulong kb;
                if (ulong.TryParse(parts[0], out kb)) return kb * 1024UL;
            }
            return null;
        }
        public static ulong AvailableBytes() {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return Windows().AvailablePhysical;
            return MemInfo("MemAvailable") ?? ulong.MaxValue;
        }
        // Commit still available (RAM + page file); no WMI, which Codex sandboxes deny.
        public static ulong AvailableCommitBytes() {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return Windows().AvailablePageFile;
            var ram = MemInfo("MemAvailable");
            if (ram == null) return ulong.MaxValue;
            return ram.Value + (MemInfo("SwapFree") ?? 0UL);
        }
    }
}
'@
}
if (-not ('HK3DS.ValidationCpu' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
namespace HK3DS {
    public static class ValidationCpu {
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
        // Idle and total ticks; false when this system has neither GetSystemTimes nor /proc/stat.
        private static bool Times(out double idle, out double total) {
            idle = 0; total = 0;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) {
                long i, k, u;
                if (!GetSystemTimes(out i, out k, out u)) throw new Win32Exception(Marshal.GetLastWin32Error());
                idle = i; total = k + u;   // kernel time includes idle
                return true;
            }
            if (!File.Exists("/proc/stat")) return false;
            var fields = File.ReadAllLines("/proc/stat")[0].Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            for (int n = 1; n < fields.Length && n <= 8; n++) {
                double value = double.Parse(fields[n], System.Globalization.CultureInfo.InvariantCulture);
                total += value;
                if (n == 4 || n == 5) idle += value;   // idle, iowait
            }
            return true;
        }
        // Percent of all logical processors busy over the sample.
        public static double BusyPercent(int sampleMilliseconds) {
            double i1, t1, i2, t2;
            if (!Times(out i1, out t1)) return 0;
            Thread.Sleep(sampleMilliseconds);
            if (!Times(out i2, out t2)) return 0;
            double total = t2 - t1;
            return total <= 0 ? 0 : 100.0 * (total - (i2 - i1)) / total;
        }
    }
}
'@
}

function Get-MachineMemoryGate {
    # The machine-local gate in the main checkout's _private/write-min-free-gb.txt (see validation-lock.ps1):
    # a number of GB of free RAM, or "commit:<GB>" of free commit (RAM + page file). 2026-11-01: check.ps1 kept
    # the 4 GB RAM default on the laptop (commit:4, ~2 GB free RAM), so every agent's Changed waited for hours.
    $checkout = Split-Path -Parent $PSScriptRoot
    $main = $checkout
    $dotGit = Join-Path $checkout '.git'
    if (Test-Path -LiteralPath $dotGit -PathType Leaf) {
        $gitDir = ((Get-Content -LiteralPath $dotGit -TotalCount 1) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $gitDir))
    }
    $file = Join-Path $main '_private/write-min-free-gb.txt'
    $gateText = if (Test-Path -LiteralPath $file) { (Get-Content -LiteralPath $file -TotalCount 1).Trim() -replace ',', '.' } else { '' }
    if ($gateText -and ($gateText -notmatch '^(?:commit:)?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)$' -or
        [double]::IsInfinity([double]($gateText -replace '^commit:', '')))) {
        throw "Invalid machine memory gate '$gateText'; expected GB or commit:<GB>."
    }
    $gateText
}

function Wait-HeavyAdmission {
    # Waits until RAM (strict) and CPU (soft, bounded) allow one more heavy command; returns a short note.
    param([double]$MinFreeGB = $(if ($env:HK3DS_WRITE_MIN_FREE_GB) { [double]$env:HK3DS_WRITE_MIN_FREE_GB }
                                 elseif ((Get-MachineMemoryGate) -match '^commit:') { 0 }
                                 elseif ((Get-MachineMemoryGate) -match '^[\d.]+$') { [double](Get-MachineMemoryGate) }
                                 else { 4 }),
          [double]$MinCommitGB = $(if ($env:HK3DS_WRITE_MIN_FREE_GB) { 0 } elseif ((Get-MachineMemoryGate) -match '^commit:([\d.]+)$') { [double]$Matches[1] } else { 0 }),
          [double]$MaxCpu = $(if ($env:HK3DS_HEAVY_MAX_CPU) { [double]$env:HK3DS_HEAVY_MAX_CPU } else { 85 }),
          [double]$MaxCpuWaitMinutes = $(if ($env:HK3DS_HEAVY_MAX_CPU_WAIT_MINUTES) { [double]$env:HK3DS_HEAVY_MAX_CPU_WAIT_MINUTES } else { 15 }),
          [datetime]$Deadline = [datetime]::MaxValue,
          [int]$PollSeconds = 10)
    # A RAM override changes admission thresholds, not machine gate validation.
    $null = Get-MachineMemoryGate
    $announced = @{}
    $cpuSince = Get-Date
    while ($true) {
        if (((Get-Date) -gt $Deadline)) { throw 'heavy-command admission timed out' }
        if (([double][HK3DS.ValidationMemory]::AvailableBytes() / 1GB) -lt $MinFreeGB) {
            if (-not $announced.ram) { Write-Output "waiting for $MinFreeGB GB free RAM"; $announced.ram = $true }
            Start-Sleep -Seconds $PollSeconds; continue
        }
        if ($MinCommitGB -and ([double][HK3DS.ValidationMemory]::AvailableCommitBytes() / 1GB) -lt $MinCommitGB) {
            if (-not $announced.commit) { Write-Output "waiting for $MinCommitGB GB free commit (RAM + page file)"; $announced.commit = $true }
            Start-Sleep -Seconds $PollSeconds; continue
        }
        $busy = [HK3DS.ValidationCpu]::BusyPercent(2000)
        if ($busy -le $MaxCpu) { return }
        if (((Get-Date) - $cpuSince).TotalMinutes -ge $MaxCpuWaitMinutes) {
            Write-Output ("CPU still {0:N0}% after {1} min; running anyway" -f $busy, $MaxCpuWaitMinutes); return
        }
        if (-not $announced.cpu) { Write-Output ("waiting for CPU below {0}% (now {1:N0}%)" -f $MaxCpu, $busy); $announced.cpu = $true }
        Start-Sleep -Seconds $PollSeconds
    }
}

function Set-HeavyPriority {
    param([ValidateSet('BelowNormal','Normal')][string]$DefaultPriority = 'BelowNormal')
    # Children inherit this priority; explicit HK3DS_HEAVY_PRIORITY always wins.
    $class = if ($env:HK3DS_HEAVY_PRIORITY) { $env:HK3DS_HEAVY_PRIORITY } else { $DefaultPriority }
    try { [Diagnostics.Process]::GetCurrentProcess().PriorityClass = $class } catch { }
}
