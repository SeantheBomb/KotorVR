# Query the Ghidra project (C:\Users\SeanF\Tools\kvr_ghidra, built from the probe's image dump; see FINDINGS.md).
#   .\tools\ghidra\q.ps1 decomp 0x5BACA0 [0x...]     decompiled C
#   .\tools\ghidra\q.ps1 xrefs 0x832874               references
#   .\tools\ghidra\q.ps1 strings 'RoundStarted'       strings + referencing functions
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$tools = 'C:\Users\SeanF\Tools'
$env:JAVA_HOME = Join-Path $tools 'jdk-25.0.4.1+1'
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
$env:TEMP = Join-Path $tools 'kvr_ghidra\tmp'; $env:TMP = $env:TEMP
$out = Join-Path $env:TEMP ('q_' + [guid]::NewGuid().ToString('N') + '.txt')
$scripts = Join-Path $PSScriptRoot 'scripts'
& (Join-Path $tools 'ghidra_12.2_PUBLIC\support\analyzeHeadless.bat') (Join-Path $tools 'kvr_ghidra') kotor -process swkotor_dump.exe -noanalysis -readOnly -scriptPath $scripts -postScript KvrQuery.java $out @Rest *> (Join-Path $env:TEMP 'q_last.log')
if (Test-Path $out) { Get-Content $out; Remove-Item $out } else { Get-Content (Join-Path $env:TEMP 'q_last.log') -Tail 20 }
