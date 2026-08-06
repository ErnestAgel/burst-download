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
    [string]$NotesFile = ""
)

$ErrorActionPreference = 'Stop'
$scriptName = Split-Path -Leaf $PSCommandPath

function Assert-LastOk {
    param([string]$What)
    if ($LASTEXITCODE -ne 0) { throw "$What 失败(退出码 $LASTEXITCODE)" }
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

# ---------- 2) 路径 ----------
$RepoRoot = Split-Path -Parent $PSScriptRoot        # scripts/ 上一级 = 仓库根
if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent $RepoRoot) 'release-assets' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# WSL 内仓库路径:F:\curlbot\curlbolt -> /mnt/f/curlbot/curlbolt
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

# ---------- 4) tag 冲突检查(防误覆盖) ----------
if (-not $SkipRelease) {
    $remote = git ls-remote --tags origin "refs/tags/$Version" 2>$null
    if ($remote) { throw "远程已存在 tag $Version,请换版本号或先删除旧 tag" }
    if (git tag -l "$Version") { throw "本地已存在 tag $Version" }
}

# ---------- 5) 构建三平台 ----------
# 5.1 Linux x86_64(WSL 原生)
Write-Host "== [1/3] Linux x86_64 Release (WSL) =="
wsl.exe -e bash -lc "cd $WslRepo && cmake -B build-rel-x64 -DCMAKE_BUILD_TYPE=Release . >/dev/null 2>&1 && cmake --build build-rel-x64 -j`$(nproc) 2>&1 | tail -2"
Assert-LastOk 'Linux x86_64 构建'
if (-not (Test-Path (Join-Path $RepoRoot 'build-rel-x64\curlbolt'))) { throw 'Linux x86_64 产物缺失' }

# 5.2 Linux aarch64(WSL 交叉编译)
Write-Host "== [2/3] Linux aarch64 Release (WSL 交叉) =="
wsl.exe -e bash -lc "cd $WslRepo && cmake -B build-rel-arm64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ . >/dev/null 2>&1 && cmake --build build-rel-arm64 -j`$(nproc) 2>&1 | tail -2"
Assert-LastOk 'Linux aarch64 构建'
if (-not (Test-Path (Join-Path $RepoRoot 'build-rel-arm64\curlbolt'))) { throw 'Linux aarch64 产物缺失' }

# 5.3 Windows x86_64(MSYS2/mingw64)
Write-Host "== [3/3] Windows x86_64 Release (MSYS2) =="
$env:Path = "$Msys2Path\mingw64\bin;$Msys2Path\usr\bin;" + $env:Path
cmake -B (Join-Path $RepoRoot 'build-win-rel') -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release $RepoRoot
Assert-LastOk 'Windows cmake 配置'
cmake --build (Join-Path $RepoRoot 'build-win-rel') -j 8
Assert-LastOk 'Windows 构建'
$WinExe = Join-Path $RepoRoot 'build-win-rel\curlbolt.exe'
if (-not (Test-Path $WinExe)) { throw 'Windows 产物缺失' }

# ---------- 6) 打包 ----------
Write-Host "== 打包产物到 $OutDir =="
Copy-Item (Join-Path $RepoRoot 'build-rel-x64\curlbolt')  (Join-Path $OutDir 'curlbolt-linux-x86_64')  -Force
Copy-Item (Join-Path $RepoRoot 'build-rel-arm64\curlbolt') (Join-Path $OutDir 'curlbolt-linux-aarch64') -Force

# Windows:exe + Python 运行 dll 打 zip
$WinBuildDir = Join-Path $RepoRoot 'build-win-rel'
$ZipPath = Join-Path $OutDir 'curlbolt-windows-x86_64.zip'
Remove-Item $ZipPath -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $WinBuildDir 'curlbolt.exe'),
    (Join-Path $WinBuildDir '*.dll') -DestinationPath $ZipPath -Force
if (-not (Test-Path $ZipPath)) { throw 'Windows zip 打包失败' }

# ---------- 7) 校验 ----------
Write-Host "== 校验产物 =="
$ok = 0
if ((wsl.exe -e bash -lc "cd $WslRepo && ./build-rel-x64/curlbolt -h 2>&1 | head -1" ) -match 'Usage') { $ok++; Write-Host "  [OK] linux-x86_64" } else { Write-Host "  [!!] linux-x86_64 运行异常" }
if ((wsl.exe -e bash -lc "file $WslRepo/build-rel-arm64/curlbolt 2>/dev/null | grep -o aarch64" ) -match 'aarch64') { $ok++; Write-Host "  [OK] linux-aarch64(架构)" } else { Write-Host "  [!!] linux-aarch64 架构异常" }
if ((& $WinExe -h 2>&1 | Select-Object -First 1) -match 'Usage') { $ok++; Write-Host "  [OK] windows-x86_64" } else { Write-Host "  [!!] windows-x86_64 运行异常" }
if ($ok -lt 3) { throw "产物校验未全部通过($ok/3)" }

Write-Host "== 产物清单 =="
Get-ChildItem $OutDir -Filter 'curlbolt-*' | Select-Object Name, Length | Format-Table -AutoSize

if ($SkipRelease) {
    Write-Host "`n[SkipRelease] 构建与打包完成,跳过 GitHub 发布。产物在: $OutDir"
    return
}

# ---------- 8) 发布到 GitHub ----------
Write-Host "== 发布 $Version 到 GitHub Releases =="

# 8.1 token(从 git 凭据提取,不硬编码)
$cred = "protocol=https`nhost=github.com`n`n" | git credential fill 2>$null
$tokenLine = ($cred -split "`n" | Where-Object { $_ -like 'password=*' } | Select-Object -First 1)
if (-not $tokenLine -or $tokenLine.Length -le 9) { throw '无法从 git 凭据获取 GitHub token(请先 git push 成功一次或设置 GITHUB_TOKEN)' }
$token = $tokenLine.Substring(9)

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
    $releaseNotes = "## curlbolt $Version`n`n### 变更记录`n$log`n`n### 平台产物`n- curlbolt-linux-x86_64 : 静态单文件`n- curlbolt-linux-aarch64 : 静态单文件(交叉编译)`n- curlbolt-windows-x86_64.zip : exe + Python 运行 dll(解压即用)"
}

# 8.3 创建 Release
$body = @{
    tag_name         = $Version
    target_commitish = 'main'
    name             = "curlbolt $Version"
    body             = $releaseNotes
    draft            = $false
    prerelease       = $false
} | ConvertTo-Json -Depth 3

$release = Invoke-RestMethod -Method Post -Uri 'https://api.github.com/repos/ErnestAgel/curlbolt/releases' `
    -Headers $headers -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($body))
Write-Host "  Release 已创建: $($release.html_url)"

# 8.4 上传三平台资产
$assets = @(
    @{ Name = 'curlbolt-linux-x86_64';    Path = Join-Path $OutDir 'curlbolt-linux-x86_64' },
    @{ Name = 'curlbolt-linux-aarch64';   Path = Join-Path $OutDir 'curlbolt-linux-aarch64' },
    @{ Name = 'curlbolt-windows-x86_64.zip'; Path = $ZipPath }
)
foreach ($a in $assets) {
    $uri = "https://uploads.github.com/repos/ErnestAgel/curlbolt/releases/$($release.id)/assets?name=$($a.Name)"
    $up = Invoke-RestMethod -Method Post -Uri $uri -Headers $headers `
        -ContentType 'application/octet-stream' -InFile $a.Path
    Write-Host "  [上传] $($up.name) ($($up.size) 字节)"
}

# 8.5 本地 tag 同步
git tag $Version
git push origin $Version
Write-Host "`n发布完成: $($release.html_url)"
