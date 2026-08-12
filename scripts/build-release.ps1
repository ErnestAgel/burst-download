#requires -Version 5.1
<#
.SYNOPSIS
一键构建三平台 (Linux x86_64 / Linux aarch64 / Windows x86_64) Release 并发布到 GitHub Releases。

.DESCRIPTION
- Linux 两平台在 WSL 内构建(x86_64 原生 + aarch64 交叉编译)
- Windows 版用 MSYS2/mingw64 构建,并自动打包 exe + Python 运行 dll 为 zip
- 产物输出到 release-assets/(默认在仓库父目录)
- 发布前检查 tag 冲突,防止误覆盖已有 Release
- 复用 git 凭据中的 GitHub token,不硬编码任何密钥

.PARAMETER Version
版本号,如 v1.2.0 或 1.2.0(自动补 v 前缀)。发布模式必填;-SkipRelease 时忽略。

.PARAMETER Msys2Path
MSYS2 安装目录(含 mingw64\bin\gcc.exe)。默认自动检测常见路径。

.PARAMETER OutDir
产物输出目录。默认 <仓库父目录>\release-assets。

.PARAMETER SkipRelease
只构建+打包+校验,不创建 GitHub Release(试运行用)。

.PARAMETER NotesFile
Release 正文 Markdown 文件路径。缺省时根据 git log 自动生成变更说明。

.PARAMETER Python311Exe
Python 3.11 可执行文件（pyc 化必需，字节码需匹配 3.11 运行时）。
缺省依次尝试环境变量 PYTHON311_EXE 与常见路径。

.PARAMETER CodeSignCert
可选：Windows Authenticode 代码签名证书（.pfx）路径。提供后 Windows exe
将用 signtool（Windows SDK）做 SHA256 + RFC3161 时间戳签名并校验。
未提供则跳过签名（产物仍可发布，但 SmartScreen 无信任）。

.PARAMETER CodeSignPassword
证书私钥密码（.pfx）。明文传参，注意脚本调用环境的可见性。

.EXAMPLE
.\scripts\build-release.ps1 v1.2.0
.\scripts\build-release.ps1 -Version 1.2.0 -SkipRelease   # 只构建不发布
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Msys2Path = "",
    [string]$OutDir = "",
    [switch]$SkipRelease,
    [string]$NotesFile = "",
    [string]$Python311Exe = "",
    [string]$CodeSignCert = "",
    [string]$CodeSignPassword = ""
)

$ErrorActionPreference = 'Stop'
$scriptName = Split-Path -Leaf $PSCommandPath

function Assert-LastOk {
    param([string]$What)
    if ($LASTEXITCODE -ne 0) { throw "$What 失败(退出码 $LASTEXITCODE)" }
}

function Invoke-CodeSign {
    param([string]$ExePath)
    if (-not $CodeSignCert) { return }
    $signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if (-not $signtool) {
        # 常见 Windows SDK 路径（按版本目录取最新）
        $kitsRoots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin",
                       "${env:ProgramFiles}\Windows Kits\10\bin")
        foreach ($r in $kitsRoots) {
            if (-not (Test-Path $r)) { continue }
            $ver = Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^\d+\.' } |
                Sort-Object Name -Descending | Select-Object -First 1
            if (-not $ver) { continue }
            $cand = Join-Path $ver.FullName 'x64\signtool.exe'
            if (Test-Path $cand) { $signtool = $cand; break }
        }
    }
    if (-not $signtool) {
        throw "未找到 signtool.exe（需安装 Windows SDK），无法执行代码签名"
    }
    $stPath = if ($signtool -is [System.Management.Automation.CommandInfo]) {
        $signtool.Source
    } else {
        $signtool
    }
    # SHA256 + RFC3161 时间戳：证书过期后签名仍有效
    & $stPath sign /fd SHA256 /tr 'http://timestamp.digicert.com' /td SHA256 `
        /f $CodeSignCert /p $CodeSignPassword $ExePath
    if ($LASTEXITCODE -ne 0) { throw "signtool 签名失败: $ExePath" }
    & $stPath verify /pa /v $ExePath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool 校验失败: $ExePath" }
    Write-Host "  [OK] Authenticode 签名完成: $ExePath"
}

# ---------- 1) 参数校验 ----------
$Version = $Version.Trim()
if (-not $SkipRelease) {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        throw "发布模式必须指定版本号,如: .\$scriptName v1.2.0"
    }
    if ($Version -notmatch '^v?\d+\.\d+\.\d+$') {
        throw "版本号格式错误: $Version (应为 v1.2.0 形式)"
    }
    if (-not $Version.StartsWith('v')) { $Version = "v$Version" }
}
# 注入二进制的版本号（不带 v 前缀）；SkipRelease 未给版本时用 CMake 默认值
$VerNoV = if ($Version) { $Version.TrimStart('v') } else { '' }
$VerArg = if ($VerNoV) { "-DBURST_VERSION=$VerNoV" } else { '' }

# ---------- 2) 路径 ----------
$RepoRoot = Split-Path -Parent $PSScriptRoot        # scripts/ 上一级 = 仓库根
if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent $RepoRoot) 'release-assets' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# WSL 内仓库路径:F:\curlbot\burst -> /mnt/f/curlbot/burst
$WslRepo = '/mnt/' + $RepoRoot.Substring(0, 1).ToLower() + ($RepoRoot.Substring(2) -replace '\\', '/')

# ---------- 3) 工具链检查 ----------
foreach ($t in @('cmake', 'wsl')) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "缺少必要命令: $t" }
}
if (-not $Msys2Path) {
    $Msys2Path = @('D:\msys2', 'C:\msys64', 'C:\msys2', 'D:\msys64') |
        Where-Object { Test-Path (Join-Path $_ 'mingw64\bin\gcc.exe') } |
        Select-Object -First 1
}
if (-not $Msys2Path) {
    throw '未找到 MSYS2(需要 mingw64\bin\gcc.exe),可用 -Msys2Path 指定'
}
if (-not (Test-Path (Join-Path $Msys2Path 'mingw64\bin\gcc.exe'))) {
    throw "MSYS2 路径无效: $Msys2Path (缺少 mingw64\bin\gcc.exe)"
}
Write-Host "[工具] MSYS2: $Msys2Path"

# Python 3.11 编译工具（B 方案 pyc 化必需）：参数 → 环境变量 → 常见路径
if (-not $Python311Exe) { $Python311Exe = $env:PYTHON311_EXE }
if (-not $Python311Exe) {
    $Python311Exe = @('F:\curlbot\tools\python-3.11.9-embed\python.exe',
                      'C:\Python311\python.exe', 'D:\Python311\python.exe') |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Python311Exe) {
    throw '缺少 Python 3.11 编译工具（B 方案 pyc 化必需）：用 -Python311Exe 指定或设置 PYTHON311_EXE'
}
Write-Host "[工具] Python311: $Python311Exe"

# ---------- 4) tag 冲突检查(防误覆盖) ----------
if (-not $SkipRelease) {
    $remote = git ls-remote --tags origin "refs/tags/$Version" 2>$null
    if ($remote) { throw "远程已存在 tag $Version,请换版本号或先删除旧 tag" }
    if (git tag -l "$Version") { throw "本地已存在 tag $Version" }

    # 4.1 提取 GitHub token —— 必须在构建(修改 PATH)之前!
    # 构建阶段会把 PATH 切到 MSYS2,此后 git credential 拿不到 Windows GCM 凭据
    $tmpCred = Join-Path $env:TEMP ("ghcred_" + [guid]::NewGuid().ToString('N') + '.txt')
    [IO.File]::WriteAllText($tmpCred, "protocol=https`nhost=github.com`n`n", [Text.Encoding]::ASCII)
    try {
        $cred = cmd /c "git credential fill < `"$tmpCred`"" 2>$null
    } finally {
        Remove-Item $tmpCred -ErrorAction SilentlyContinue
    }
    $tokenLine = ($cred | Where-Object { $_ -like 'password=*' } | Select-Object -First 1)
    if (-not $tokenLine -or $tokenLine.Length -le 9) { throw '无法从 git 凭据获取 GitHub token(请先 git push 成功一次或设置 GITHUB_TOKEN)' }
    $token = $tokenLine.Substring(9)
}

# ---------- 5) 构建三平台 ----------
# 5.1 Linux x86_64(WSL 原生)
Write-Host "== [1/3] Linux x86_64 Release (WSL) =="
wsl.exe -e bash -lc "set -o pipefail; cd $WslRepo && cmake -B build-rel-x64 -DCMAKE_BUILD_TYPE=Release $VerArg . >/dev/null 2>&1 && cmake --build build-rel-x64 -j`$(nproc) 2>&1 | tail -2"
Assert-LastOk 'Linux x86_64 构建'
if (-not (Test-Path (Join-Path $RepoRoot 'build-rel-x64\burst'))) { throw 'Linux x86_64 产物缺失' }

# 5.2 Linux aarch64(WSL 交叉编译)
Write-Host "== [2/3] Linux aarch64 Release (WSL 交叉) =="
# aarch64 为 CLI-only：交叉环境无 aarch64 版 X11/GL 库，显式关闭 GUI（与 README 一致）
wsl.exe -e bash -lc "set -o pipefail; cd $WslRepo && cmake -B build-rel-arm64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ -DBUILD_GUI=OFF $VerArg . >/dev/null 2>&1 && cmake --build build-rel-arm64 -j`$(nproc) 2>&1 | tail -2"
Assert-LastOk 'Linux aarch64 构建'
if (-not (Test-Path (Join-Path $RepoRoot 'build-rel-arm64\burst'))) { throw 'Linux aarch64 产物缺失' }

# 5.3 Windows x86_64(MSYS2/mingw64)
Write-Host "== [3/3] Windows x86_64 Release (MSYS2) =="
$env:Path = "$Msys2Path\mingw64\bin;$Msys2Path\usr\bin;" + $env:Path
cmake -B (Join-Path $RepoRoot 'build-win-rel') -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release $VerArg $RepoRoot
Assert-LastOk 'Windows cmake 配置'
cmake --build (Join-Path $RepoRoot 'build-win-rel') -j 8
Assert-LastOk 'Windows 构建'
$WinExe = Join-Path $RepoRoot 'build-win-rel\burst.exe'
if (-not (Test-Path $WinExe)) { throw 'Windows 产物缺失' }

# ---------- 辅助：运行时 pyc 化与发布打包 ----------
# 运行时全量 pyc 化删除 .py 源码；python311.dll 改名 bd311.dll 并修补导入名。
function Set-BurstImportName {
    param([string]$Path)
    $old = [Text.Encoding]::ASCII.GetBytes('python311.dll')
    $new = [Text.Encoding]::ASCII.GetBytes('bd311.dll')
    $bytes = [IO.File]::ReadAllBytes($Path)
    $changed = $false
    for ($i = 0; $i -le $bytes.Length - $old.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $old.Length; $j++) {
            if ($bytes[$i + $j] -ne $old[$j]) { $match = $false; break }
        }
        if ($match) {
            for ($j = 0; $j -lt $new.Length; $j++) { $bytes[$i + $j] = $new[$j] }
            for ($j = $new.Length; $j -lt $old.Length; $j++) { $bytes[$i + $j] = 0 }
            $changed = $true
            $i += $old.Length - 1
        }
    }
    if ($changed) {
        [IO.File]::WriteAllBytes($Path, $bytes)
        Write-Host ("  [OK] 修补导入名: " + (Split-Path -Leaf $Path))
    }
}

function ConvertTo-PycOnlyRuntime {
    param(
        [string]$Src,
        [string]$Dst,
        [string]$PyExe,
        [string]$Platform  # windows=留 .pyd 去 .so；linux=留 .so 去 .pyd；linux-arm64=两者都去
    )
    New-Item -ItemType Directory -Force -Path $Dst | Out-Null
    Copy-Item (Join-Path $Src '*') $Dst -Recurse -Force
    & $PyExe -m compileall -q -f -b $Dst 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'pyc 编译失败（Python 版本需与 3.11 匹配）' }
    Get-ChildItem $Dst -Recurse -File -Filter '*.py' |
        Where-Object { $_.FullName -notlike '*\yt_dlp\version.py' -and `
                       $_.FullName -notlike '*/yt_dlp/version.py' } | Remove-Item -Force
    Get-ChildItem $Dst -Recurse -Directory -Filter '__pycache__' | Remove-Item -Recurse -Force
    if ($Platform -eq 'windows') {
        Get-ChildItem $Dst -Recurse -File -Filter '*.so' | Remove-Item -Force
    } elseif ($Platform -eq 'linux') {
        Get-ChildItem $Dst -Recurse -File -Filter '*.pyd' | Remove-Item -Force
    } else {
        # linux-arm64：仓库内扩展模块为 x86_64 构建，aarch64 不携带任何原生扩展
        Get-ChildItem $Dst -Recurse -File -Include '*.pyd', '*.so' | Remove-Item -Force
    }
    Get-ChildItem $Dst -Recurse -File -Filter '.parser_last_check' | Remove-Item -Force
    Get-ChildItem $Dst -Recurse -File -Filter '.result_*.json' | Remove-Item -Force
}

function Add-BurstEmbeddedRuntime {
    param(
        [string]$ExePath,
        [string]$AssetsDir,
        [string]$PyExe
    )
    $blob = Join-Path $env:TEMP ("burst-blob-" + [guid]::NewGuid().ToString('N') + '.bin')
    try {
        $res = (& $PyExe (Join-Path $RepoRoot 'scripts\make_runtime_blob.py') `
                    $AssetsDir $blob 2>$null | Select-Object -Last 1)
        $parts = ($res -split '\s+')
        if ($parts.Count -ne 2) { throw "运行时打包生成失败: $res" }
        $hash = [Convert]::ToUInt64($parts[0], 16)
        $data = [IO.File]::ReadAllBytes($blob)
        $fs = [IO.File]::Open($ExePath, [IO.FileMode]::Append)
        try {
            $fs.Write($data, 0, $data.Length)
            $footer = [Text.Encoding]::ASCII.GetBytes('BURSTARC')
            $footer += [BitConverter]::GetBytes([uint64]$data.Length)
            $footer += [BitConverter]::GetBytes($hash)
            $footer += [Text.Encoding]::ASCII.GetBytes('BURSTEND')
            if ($footer.Length -ne 32) { throw "footer 长度异常: $($footer.Length)" }
            $fs.Write($footer, 0, $footer.Length)
        } finally {
            $fs.Close()
        }
        $fs2 = [IO.File]::Open($ExePath, [IO.FileMode]::Open, [IO.FileAccess]::Read)
        try {
            $fs2.Seek(-32, [IO.SeekOrigin]::End) | Out-Null
            $tail = New-Object byte[] 32
            [void]$fs2.Read($tail, 0, 32)
            if ([Text.Encoding]::ASCII.GetString($tail, 0, 8) -ne 'BURSTARC') {
                throw '运行时资源写入校验失败'
            }
        } finally {
            $fs2.Close()
        }
        Write-Host ("  [OK] 打包运行时 -> {0} (+{1:N1} MB)" -f `
            (Split-Path -Leaf $ExePath), ($data.Length / 1MB))
    } finally {
        Remove-Item $blob -ErrorAction SilentlyContinue
    }
}

function New-BurstWindowsZip {
    param(
        [string]$ExeName,   # 如 burst.exe / burst-gui.exe
        [string]$BuildDir,  # build-win-rel
        [string]$ZipPath,   # 输出 zip 完整路径
        [string]$RuntimeDir,# third_party/python/runtime
        [string]$PyExe,     # Python 3.11 可执行文件（pyc 化）
        [string]$Label      # 日志前缀
    )
    $ExePath = Join-Path $BuildDir $ExeName
    if (-not (Test-Path $ExePath)) { throw "$Label 缺少产物: $ExePath" }
    if (-not (Test-Path $PyExe)) {
        throw "$Label 缺少 Python 3.11 编译工具: $PyExe（用 -Python311Exe 指定或设置 PYTHON311_EXE）"
    }
    foreach ($req in @('stdlib', 'yt_dlp')) {
        if (-not (Test-Path (Join-Path $RuntimeDir $req))) {
            throw "$Label 缺少 Python 运行时资源: $(Join-Path $RuntimeDir $req)"
        }
    }

    # 暂存目录：exe/dll 平铺；assets 仅作为打包素材，最终不进 zip
    $StageDir = Join-Path $BuildDir ("stage-" + [IO.Path]::GetFileNameWithoutExtension($ExeName))
    if (Test-Path $StageDir) { Remove-Item -LiteralPath $StageDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

    # 1) 运行时 pyc 化（B 方案）到暂存 assets
    ConvertTo-PycOnlyRuntime -Src $RuntimeDir -Dst (Join-Path $StageDir 'assets') `
        -PyExe $PyExe -Platform 'windows'
    # 2) 修补 assets/lib-dynload 内 .pyd 的导入名（python311.dll → bd311.dll）
    Get-ChildItem (Join-Path $StageDir 'assets\lib-dynload') -File -Filter '*.pyd' `
        -ErrorAction SilentlyContinue | ForEach-Object { Set-BurstImportName $_.FullName }
    # 3) exe 副本：修补导入名 + 打包运行时
    Copy-Item $ExePath (Join-Path $StageDir $ExeName)
    Set-BurstImportName (Join-Path $StageDir $ExeName)
    Add-BurstEmbeddedRuntime -ExePath (Join-Path $StageDir $ExeName) `
        -AssetsDir (Join-Path $StageDir 'assets') -PyExe $PyExe
    Invoke-CodeSign (Join-Path $StageDir $ExeName)
    # 4) dll：python311.dll → bd311.dll；assets 不再随包
    Copy-Item (Join-Path $BuildDir '*.dll') $StageDir
    if (Test-Path (Join-Path $StageDir 'python311.dll')) {
        Move-Item (Join-Path $StageDir 'python311.dll') (Join-Path $StageDir 'bd311.dll') -Force
    }
    Remove-Item (Join-Path $StageDir 'assets') -Recurse -Force

    # 5) 压缩
    Remove-Item $ZipPath -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $StageDir '*') -DestinationPath $ZipPath -Force
    if (-not (Test-Path $ZipPath)) { throw "$Label zip 打包失败: $ZipPath" }
    Remove-Item -LiteralPath $StageDir -Recurse -Force

    # 6) 校验：zip 只含 exe + dll；不得含 assets/运行时文件/python311.dll
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $names = $zip.Entries | ForEach-Object { $_.FullName }
        if (-not ($names -contains 'bd311.dll')) {
            throw "$Label zip 缺少 bd311.dll（python311.dll 改名失败），禁止上传"
        }
        if ($names -contains 'python311.dll') {
            throw "$Label zip 仍包含 python311.dll（改名失败），禁止上传"
        }
        if ($names | Where-Object { $_ -like 'assets/*' -or $_ -like 'assets\*' -or `
                $_ -like '*.py' -or $_ -like '*.so' -or $_ -like '*.pyd' -or $_ -like '*.pyc' }) {
            throw "$Label zip 不应包含运行时文件，禁止上传"
        }
        $sizeMB = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
        Write-Host ("  [OK] {0} -> {1} ({2} MB, {3} 条目, 运行时已打包)" -f `
            $Label, $ZipPath, $sizeMB, $zip.Entries.Count)
    } finally {
        $zip.Dispose()
    }
}

# ---------- 6) 打包 ----------
Write-Host "== 打包产物到 $OutDir =="

# Linux x86_64 / aarch64：单文件发布
$PyRuntimeDir = Join-Path $RepoRoot 'third_party\python\runtime'
$LinuxAssets = Join-Path $env:TEMP ("burst-linux-assets-" + [guid]::NewGuid().ToString('N'))
ConvertTo-PycOnlyRuntime -Src $PyRuntimeDir -Dst $LinuxAssets -PyExe $Python311Exe -Platform 'linux'
Copy-Item (Join-Path $RepoRoot 'build-rel-x64\burst') (Join-Path $OutDir 'burst-linux-x86_64') -Force
Add-BurstEmbeddedRuntime -ExePath (Join-Path $OutDir 'burst-linux-x86_64') `
    -AssetsDir $LinuxAssets -PyExe $Python311Exe
Remove-Item $LinuxAssets -Recurse -Force
# aarch64：剔除全部原生扩展（纯 pyc）
$ArmAssets = Join-Path $env:TEMP ("burst-arm-assets-" + [guid]::NewGuid().ToString('N'))
ConvertTo-PycOnlyRuntime -Src $PyRuntimeDir -Dst $ArmAssets -PyExe $Python311Exe -Platform 'linux-arm64'
Copy-Item (Join-Path $RepoRoot 'build-rel-arm64\burst') (Join-Path $OutDir 'burst-linux-aarch64') -Force
Add-BurstEmbeddedRuntime -ExePath (Join-Path $OutDir 'burst-linux-aarch64') `
    -AssetsDir $ArmAssets -PyExe $Python311Exe
Remove-Item $ArmAssets -Recurse -Force

# Windows: burst.exe + Python 运行 dll 打 zip，解压即用
$WinBuildDir = Join-Path $RepoRoot 'build-win-rel'
$ZipPath = Join-Path $OutDir 'burst-windows-x86_64.zip'
New-BurstWindowsZip -ExeName 'burst.exe' -BuildDir $WinBuildDir `
    -ZipPath $ZipPath -RuntimeDir $PyRuntimeDir -PyExe $Python311Exe -Label 'burst-windows-x86_64'

# ---------- 7) 校验 ----------
Write-Host "== 校验产物 =="
$ok = 0
if ((wsl.exe -e bash -lc "cd $WslRepo && ./build-rel-x64/burst -h 2>&1 | head -1" ) -match 'Usage') { $ok++; Write-Host "  [OK] linux-x86_64" } else { Write-Host "  [!!] linux-x86_64 运行异常" }
if ((wsl.exe -e bash -lc "file $WslRepo/build-rel-arm64/burst 2>/dev/null | grep -o aarch64" ) -match 'aarch64') { $ok++; Write-Host "  [OK] linux-aarch64(架构)" } else { Write-Host "  [!!] linux-aarch64 架构异常" }
if ((& $WinExe -h 2>&1 | Select-Object -First 1) -match 'Usage') { $ok++; Write-Host "  [OK] windows-x86_64" } else { Write-Host "  [!!] windows-x86_64 运行异常" }
if ($ok -lt 3) { throw "产物校验未全部通过($ok/3)" }

Write-Host "== 产物清单 =="
Get-ChildItem $OutDir -Filter 'burst-*' | Select-Object Name, Length | Format-Table -AutoSize

Write-Host "== 生成 SHA256 哈希清单 =="
Get-ChildItem $OutDir -File | Where-Object { $_.Name -like 'burst-*' } |
    ForEach-Object {
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
        "{0}  {1}" -f $h, $_.Name
    } | Set-Content -LiteralPath (Join-Path $OutDir 'SHA256SUMS.txt') -Encoding ASCII
Write-Host "  [OK] SHA256SUMS.txt（供用户/更新器校验产物完整性）"

if ($SkipRelease) {
    Write-Host "`n[SkipRelease] 构建与打包完成,跳过 GitHub 发布。产物在: $OutDir"
    return
}

# ---------- 8) 发布到 GitHub ----------
Write-Host "== 发布 $Version 到 GitHub Releases =="

# 8.1 token 已在步骤 4.1 提前提取(必须在构建修改 PATH 之前),此处直接使用

$headers = @{
    Authorization = "Bearer $token"
    Accept        = 'application/vnd.github+json'
}

# 8.2 Release 正文:优先 -NotesFile,否则按 git log 自动生成
if ($NotesFile -and (Test-Path $NotesFile)) {
    $releaseNotes = Get-Content $NotesFile -Raw -Encoding UTF8
} else {
    $prevTag = (git tag --sort=-version:refname | Where-Object { $_ -match '^v\d+\.\d+\.\d+$' } | Select-Object -First 1)
    $log = if ($prevTag) { (git log "$prevTag..HEAD" --oneline | Out-String).Trim() } else { (git log --oneline -10 | Out-String).Trim() }
    $releaseNotes = "## burst $Version`n`n### 变更记录`n$log`n`n### 平台产物`n- burst-linux-x86_64 : 单文件（curl/Python/FFmpeg 静态，依赖桌面 libGL/X11）`n- burst-linux-aarch64 : 交叉编译`n- burst-windows-x86_64.zip : burst.exe + Python 运行 dll（解压即用）"
}

# 8.3 创建 Release
$body = @{
    tag_name         = $Version
    target_commitish = 'main'
    name             = "burst $Version"
    body             = $releaseNotes
    draft            = $false
    prerelease       = $false
} | ConvertTo-Json -Depth 3

$release = Invoke-RestMethod -Method Post -Uri 'https://api.github.com/repos/ErnestAgel/burst-download/releases' `
    -Headers $headers -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($body))
Write-Host "  Release 已创建: $($release.html_url)"

# 8.4 上传三平台资产
$assets = @(
    @{ Name = 'burst-linux-x86_64';    Path = Join-Path $OutDir 'burst-linux-x86_64' },
    @{ Name = 'burst-linux-aarch64';   Path = Join-Path $OutDir 'burst-linux-aarch64' },
    @{ Name = 'burst-windows-x86_64.zip'; Path = $ZipPath }
)
foreach ($a in $assets) {
    $uri = "https://uploads.github.com/repos/ErnestAgel/burst-download/releases/$($release.id)/assets?name=$($a.Name)"
    $up = Invoke-RestMethod -Method Post -Uri $uri -Headers $headers `
        -ContentType 'application/octet-stream' -InFile $a.Path
    Write-Host "  [上传] $($up.name) ($($up.size) 字节)"
}

# 8.5 本地 tag 同步
git tag $Version
git push origin $Version
Write-Host "`n发布完成: $($release.html_url)"
