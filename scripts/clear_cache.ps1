#requires -Version 5.1
<#
.SYNOPSIS
删除可重新生成的前端、Bazel 与 Tauri 编译产物及工具缓存（Windows）。

.DESCRIPTION
与 scripts/clear_cache.sh 语义保持一致：默认执行清理；传入 -DryRun 仅打印将删除的路径。
固定以本脚本所在目录的上级（项目根目录）为操作基准，避免从其他目录调用时误删同名路径。

对 Bazel 输出目录 bazel-* 使用非递归的 .NET Directory.Delete，避免 Remove-Item -Recurse
递归删除 junction/symlink 指向的真实内容。
#>
[CmdletBinding()]
param(
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$ROOT = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Remove-PathItem {
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )
    if (-not (Test-Path -LiteralPath $Path) -and -not [System.IO.Directory]::Exists($Path) -and -not [System.IO.File]::Exists($Path)) {
        Write-Host "[clean] skip: $($Path.Substring($ROOT.Length).TrimStart('\','/'))"
        return
    }

    $rel = $Path.Substring($ROOT.Length).TrimStart('\', '/')

    if ($DryRun) {
        Write-Host "[clean] would remove: $rel"
        return
    }

    try {
        $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
        $isReparse = $item -and $item.Attributes.HasFlag([System.IO.FileAttributes]::ReparsePoint)

        if ($isReparse) {
            # 只删链接本身，不动链接目标内容。
            [System.IO.Directory]::Delete($Path, $false)
        }
        elseif (Test-Path -LiteralPath $Path -PathType Container) {
            Remove-Item -LiteralPath $Path -Recurse -Force
        }
        else {
            Remove-Item -LiteralPath $Path -Force
        }
        Write-Host "[clean] removed: $rel"
    }
    catch {
        Write-Host "[clean] ERROR removing $rel : $($_.Exception.Message)" -ForegroundColor Red
    }
}

# 与 scripts/clear_cache.sh 的 ARTIFACTS 保持一致（单一事实源）。
$ARTIFACTS = @(
    'dist'
    'node_modules\.vite'
    'src-tauri\target'
    'build'
    'bazel-bin'
    'bazel-out'
    'bazel-testlogs'
    'bazel-threejs-viz'
    'nohup.out'
    'test.log'
)

# Bazel 的 bazel-* 通常只是输出符号链接，需执行 clean 才会清除真实输出树。
$bazel = Get-Command bazel -ErrorAction SilentlyContinue
if ($bazel) {
    if ($DryRun) {
        Write-Host '[clean] would run: bazel clean'
    }
    else {
        Write-Host '[clean] running: bazel clean'
        Push-Location $ROOT
        try { & bazel clean }
        finally { Pop-Location }
    }
}
else {
    Write-Host '[clean] skip: bazel clean (bazel command not found)'
}

foreach ($artifact in $ARTIFACTS) {
    Remove-PathItem -Path (Join-Path $ROOT $artifact)
}

# TypeScript 增量编译信息可能位于根目录或子项目中（跳过 node_modules）。
$tsbuildinfo = Get-ChildItem -Path $ROOT -Filter '*.tsbuildinfo' -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notlike '*\node_modules\*' }
foreach ($f in $tsbuildinfo) {
    Remove-PathItem -Path $f.FullName
}

if ($DryRun) {
    Write-Host '[clean] dry run complete'
}
else {
    Write-Host '[clean] complete'
}
