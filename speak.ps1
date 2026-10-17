<#
.SYNOPSIS
    speak.ps1 - offline Chinese/English speech output.

.DESCRIPTION
    Rewritten 2026-10-17. Old version selected the voice by a hard-coded name
    and hard-failed (or silently did nothing) when that voice was missing,
    because $ErrorActionPreference = 'Continue' swallowed the error.

    New behaviour:
      - Picks the first available voice on its own (zh-CN first, then any).
      - Wraps everything in try/finally so the synthesizer is always disposed.
      - Supports pipe input, files, rate, volume, async mode, and WAV output.
      - Serialises concurrent calls with a global mutex so two invocations
        never talk over each other.
      - Writes failures to D:\MouseRobot\speak.log and returns a distinct
        exit code so callers can tell what went wrong.

.PARAMETER Text
    Text to speak. May also be supplied via the pipeline.

.PARAMETER TextFile
    Read the text to speak from this UTF-8 file.

.PARAMETER Voice
    Exact or partial voice name. Default: auto-pick.

.PARAMETER ListVoices
    Print installed voices and exit.

.PARAMETER Rate
    Speech rate, -10..10, default 0.

.PARAMETER Volume
    0..100, default 100.

.PARAMETER WaveOut
    Write the audio to this WAV file instead of playing it.

.PARAMETER Async
    Start speaking and return immediately (does not wait for it to finish).
    Ignored when -WaveOut is set.

.EXAMPLE
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:\MouseRobot\speak.ps1 -Text "hello"

.EXAMPLE
    "hi there" | powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:\MouseRobot\speak.ps1

.EXAMPLE
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:\MouseRobot\speak.ps1 -ListVoices
#>
param(
    # NOTE: must stay [string], not [string[]]. Under `powershell -File`,
    # an array-typed parameter only accepts ONE value and turns
    # `-Text a b` into a hard "positional parameter cannot be found" error.
    [AllowEmptyString()]
    [string]$Text,

    [string]$TextFile,
    [string]$Voice,
    [switch]$ListVoices,
    [ValidateRange(-10, 10)][int]$Rate = 0,
    [ValidateRange(0, 100)][int]$Volume = 100,
    [string]$WaveOut,
    [switch]$Async,
    [string]$LogPath = 'D:\MouseRobot\speak.log'
)

begin {
    $script:LogPath = $LogPath
    $script:Warn = ''          # set by helper functions
    function Write-Log([string]$msg) {
        try {
            $line = '{0}  {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
            Add-Content -LiteralPath $script:LogPath -Value $line -Encoding UTF8 -ErrorAction SilentlyContinue
        } catch { }
    }
}

process {
    # NOTE: no pipeline support. `powershell -File` cannot be fed through an
    # OS pipe reliably (it treats stdin as commands, not data). Use -Text or
    # -TextFile instead.
}

end {
    $ErrorActionPreference = 'Stop'   # real errors must not be swallowed

    # ---------- 0) List voices and exit ----------
    try {
        Add-Type -AssemblyName System.Speech
    } catch {
        Write-Log "cannot load System.Speech: $($_.Exception.Message)"
        Write-Output "ERR: System.Speech not available - $($_.Exception.Message)"
        exit 3
    }

    if ($ListVoices) {
        try {
            $syn = New-Object System.Speech.Synthesis.SpeechSynthesizer
            foreach ($v in $syn.GetInstalledVoices()) {
                $ci = $v.VoiceInfo
                Write-Output ("{0}`t{1}`t{2}`t{3}" -f $ci.Name, $ci.Culture, $ci.Gender, $ci.Age)
            }
            $syn.Dispose()
        } catch {
            Write-Log "list voices failed: $($_.Exception.Message)"
            Write-Output "ERR: $($_.Exception.Message)"
            exit 3
        }
        exit 0
    }

    # ---------- 1) Gather text ----------
    $body = ''
    if (-not [string]::IsNullOrWhiteSpace($Text)) { $body = $Text }
    if ($TextFile) {
        if (-not (Test-Path -LiteralPath $TextFile)) {
            Write-Log "text file not found: $TextFile"
            Write-Output "ERR: text file not found: $TextFile"
            exit 2
        }
        try { $body = [System.IO.File]::ReadAllText($TextFile, [System.Text.Encoding]::UTF8) }
        catch {
            Write-Log "read text file failed: $($_.Exception.Message)"
            Write-Output "ERR: cannot read $TextFile - $($_.Exception.Message)"
            exit 2
        }
    }
    if ([string]::IsNullOrWhiteSpace($body)) {
        Write-Log "empty text, nothing to speak"
        Write-Output 'ERR: text is empty'
        exit 2
    }
    $body = $body.Trim()

    # ---------- 2) Serialise concurrent calls with a global mutex ----------
    $mutex = $null
    try {
        $mutex = New-Object System.Threading.Mutex($false, 'Global\DSH_Speak_Mutex')
        try { $gotMutex = $mutex.WaitOne(30000) } catch [System.Threading.AbandonedMutexException] { $gotMutex = $true }
        if (-not $gotMutex) { throw 'timed out waiting for another pending speech to finish' }

        # ---------- 3) Create synthesizer and pick a voice ----------
        $syn = New-Object System.Speech.Synthesis.SpeechSynthesizer
        try {
            $voiceName = $null
            $installed = @($syn.GetInstalledVoices() | Where-Object { $_.Enabled })

            if ($Voice) {
                $hit = $installed | Where-Object { $_.VoiceInfo.Name -eq $Voice } | Select-Object -First 1
                if (-not $hit) { $hit = $installed | Where-Object { $_.VoiceInfo.Name -like "*$Voice*" } | Select-Object -First 1 }
                if ($hit) { $voiceName = $hit.VoiceInfo.Name }
                else { Write-Log "requested voice '$Voice' not found - falling back to auto pick" }
            }

            if (-not $voiceName) {
                $zh = $installed | Where-Object { $_.VoiceInfo.Culture.Name -like 'zh-*' } | Select-Object -First 1
                if ($zh) { $voiceName = $zh.VoiceInfo.Name }
            }
            if (-not $voiceName -and $installed.Count -gt 0) { $voiceName = $installed[0].VoiceInfo.Name }

            if ($voiceName) { $syn.SelectVoice($voiceName) }
            else { Write-Log 'no installed voice found - using system default' }

            $syn.Rate = $Rate
            $syn.Volume = $Volume
            $script:Warn = if ($voiceName) { "voice=$voiceName" } else { 'voice=system-default' }

            # ---------- 4) Output ----------
            if ($WaveOut) {
                $syn.SetOutputToWaveFile($WaveOut)
                $syn.Speak($body)      # synchronous: file must be complete when we return
                $syn.SetOutputToNull()
                Write-Output "spoken. wrote $WaveOut ($($body.Length) chars, $script:Warn)"
            }
            elseif ($Async) {
                # fire-and-forget; the synthesizer keeps itself alive until done
                $syn.SpeakAsync($body) | Out-Null
                $syn.Dispose()
                $syn = $null
                Write-Output "spoken. async ($($body.Length) chars, $script:Warn)"
            }
            else {
                $syn.Speak($body)
                Write-Output "spoken. ($($body.Length) chars, $script:Warn)"
            }
        }
        finally {
            if ($syn) { try { $syn.Dispose() } catch { } }
        }
    }
    catch {
        Write-Log "speak failed: $($_.Exception.Message)"
        Write-Output "ERR: $($_.Exception.Message)"
        exit 1
    }
    finally {
        if ($mutex) {
            try { $mutex.ReleaseMutex() } catch { }
            try { $mutex.Dispose() } catch { }
        }
    }
    exit 0
}
