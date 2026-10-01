param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [string]$Destination = "dist/windows-runtime-smoke",
    [string]$LogPath = "dist/windows-runtime-smoke.log"
)
$ErrorActionPreference = "Stop"
# A fresh child directory prevents an earlier extraction from satisfying missing
# dependencies when this verification is repeated against a rebuilt archive.
$extraction = Join-Path $Destination ([guid]::NewGuid().ToString("N"))
Expand-Archive -LiteralPath $Archive -DestinationPath $extraction
$executables = @(Get-ChildItem -Path $extraction -Filter niconeon.exe -Recurse)
if ($executables.Count -ne 1) { throw "Expected one packaged niconeon.exe" }
$executable = $executables[0].FullName
$appDirectory = Split-Path -Parent $executable
foreach ($name in @("opengl32sw.dll", "libgallium_wgl.dll", "licenses/software-opengl/inventory.tsv")) {
    if (-not (Test-Path -LiteralPath (Join-Path $appDirectory $name))) {
        throw "Required OpenGL fallback artifact is missing: $name"
    }
}
if (Test-Path -LiteralPath (Join-Path $appDirectory "opengl32.dll")) {
    throw "The bundle must not override the system hardware OpenGL loader"
}

Add-Type @"
using System.Runtime.InteropServices;
public static class NiconeonProcessErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
"@
# Only this process and its child inherit these error-dialog flags. The previous
# mode is restored below; no machine-wide or security setting is changed.
$previousMode = [NiconeonProcessErrorMode]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x8000)
try {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $executable
    $start.WorkingDirectory = $appDirectory
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment["PATH"] = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($name in @("QT_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                         "QT_QUICK_BACKEND", "QSG_RHI_BACKEND", "QT_OPENGL", "QT_OPENGL_DLL", "QT_OPENGL_BUGLIST",
                         "QT_NO_OPENGL_BUGLIST", "QSG_RHI_PREFER_SOFTWARE_RENDERER", "QT_LOGGING_CONF",
                         "LIBGL_ALWAYS_SOFTWARE", "GALLIUM_DRIVER", "MESA_LOADER_DRIVER_OVERRIDE",
                         "NICONEON_AUTO_VIDEO_PATH",
                         "NICONEON_SYNTHETIC_COMMENTS", "NICONEON_AUTO_PERF_LOG", "NICONICO_COOKIE",
                         "NICONEON_NICONICO_COOKIE")) {
        $null = $start.Environment.Remove($name)
    }
    $start.Environment["NICONEON_AUTO_EXIT_MS"] = "1500"
    # Exercise the actual native Windows platform with Qt's normal hardware-first
    # OpenGL selection. Mesa is available through the standard automatic fallback.
    $start.Environment["QT_QPA_PLATFORM"] = "windows"
    $start.Environment["QT_FORCE_STDERR_LOGGING"] = "1"
    $start.Environment["QT_DEBUG_PLUGINS"] = "1"
    $start.Environment["QSG_INFO"] = "1"
    $start.Environment["QT_LOGGING_RULES"] = "qt.qpa.gl=true;qt.scenegraph.general=true"
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw "Could not start packaged application" }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit(30000)
    if ($timedOut) {
        $process.Kill($true)
        $process.WaitForExit()
    }
    $exitCode = $process.ExitCode
    $hex = "0x{0:X8}" -f ([long]$exitCode -band 0xffffffffL)
    $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    $text += "`nPACKAGED_APP_EXIT=$exitCode ($hex) TIMED_OUT=$timedOut`n"
    $text | Set-Content -LiteralPath $LogPath -Encoding utf8
    Write-Host $text
    if ($timedOut) { throw "Packaged application did not exit within 30 seconds; inspect the plugin/startup log" }
    if ($exitCode -ne 0) {
        if ($hex -eq "0xC0000135") { throw "Windows loader could not resolve a required DLL ($hex)" }
        if ($hex -eq "0xC000007B") { throw "Windows loader rejected a DLL image/architecture ($hex)" }
        throw "Packaged application failed with exit code $exitCode ($hex)"
    }
    if ($text -match "Failed to create (?:RHI|QRhi)|Failed to initialize graphics backend|Failed to load opengl32sw") {
        throw "Packaged application reported graphics initialization failure"
    }
    if ($text -notmatch "Creating QRhi with backend OpenGL" -or $text -notmatch "Created QRhi") {
        throw "Packaged application did not confirm the required OpenGL scenegraph backend"
    }
} finally {
    $null = [NiconeonProcessErrorMode]::SetErrorMode($previousMode)
}
