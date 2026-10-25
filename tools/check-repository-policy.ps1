param(
    [switch]$IncludeIgnored,
    [switch]$StagedOnly
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $repositoryRoot
$previousOutputEncoding = [Console]::OutputEncoding
try {
    if ($StagedOnly -and $IncludeIgnored) {
        throw "StagedOnly and IncludeIgnored cannot be combined."
    }
    # Git's quoted display paths are not filesystem paths. NUL records also
    # preserve spaces and newlines; UTF-8 works in Windows PowerShell 5.1.
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    if ($StagedOnly) {
        $arguments = @("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z")
    } else {
        $arguments = @("ls-files", "--cached", "--others", "-z")
        if (-not $IncludeIgnored) {
            $arguments += "--exclude-standard"
        }
    }
    $rawPaths = (& git @arguments) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to enumerate repository files."
    }
    $files = @($rawPaths.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })

    $textExtensions = @(
        ".c", ".cc", ".cjs", ".cmd", ".conf", ".cpp", ".css", ".example",
        ".h", ".hpp", ".html", ".ini", ".js", ".json", ".md", ".ps1", ".py",
        ".rs", ".service", ".sh", ".tcl", ".toml", ".ts", ".tsx", ".txt",
        ".yaml", ".yml"
    )
    $violations = [System.Collections.Generic.List[string]]::new()
    $caseInsensitiveCollisions = @(
        $files |
            Group-Object { $_.ToLowerInvariant() } |
            Where-Object { $_.Count -gt 1 }
    )
    foreach ($collision in $caseInsensitiveCollisions) {
        $violations.Add(
            "case-insensitive path collision: $($collision.Group -join ', ')"
        )
    }
    foreach ($relativePath in $files) {
        if ($relativePath.Length -gt 220) {
            $violations.Add(
                "$relativePath`: relative path exceeds the 220-character publication limit"
            )
        }
    }
    $secretPatterns = @(
        @{
            Name = "GitHub token"
            Pattern = 'github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,}'
        },
        @{
            Name = "AWS access key"
            Pattern = 'AKIA[0-9A-Z]{16}'
        },
        @{
            Name = "private key"
            Pattern = '-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----'
        },
        @{
            Name = "literal bearer token"
            Pattern = 'Authorization\s*[:=]\s*["'']?Bearer\s+[A-Za-z0-9._-]{20,}'
        },
        @{
            Name = "literal surrogate token"
            Pattern = 'FPGA_SURROGATE_SERVER_TOKEN\s*=\s*["'']?[A-Za-z0-9+/_=-]{24,}'
        }
    )
    foreach ($relativePath in $files) {
        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            continue
        }
        $fullPath = Join-Path $repositoryRoot $relativePath
        if ($StagedOnly) {
            # Check the bytes that will be committed, not unstaged replacements.
            $fileSize = & git cat-file -s ":$relativePath"
            if ($LASTEXITCODE -ne 0) {
                throw "Unable to read staged file size: $relativePath"
            }
        } else {
            if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
                continue
            }
            $fileSize = (Get-Item -LiteralPath $fullPath).Length
        }
        if ([long]$fileSize -ge 90MB) {
            $violations.Add(
                ("{0}: file is at least 90 MiB and unsafe for normal GitHub publication" -f
                    $relativePath)
            )
        }
        $extension = [IO.Path]::GetExtension($relativePath).ToLowerInvariant()
        if ($extension -notin $textExtensions) {
            continue
        }
        if ($StagedOnly) {
            $lines = & git show ":$relativePath"
            if ($LASTEXITCODE -ne 0) {
                throw "Unable to read staged file: $relativePath"
            }
        } else {
            $lines = Get-Content -LiteralPath $fullPath -Encoding UTF8
        }
        $lineNumber = 0
        foreach ($line in $lines) {
            $lineNumber += 1
            $windowsAbsolute = $line -match '(?<![A-Za-z0-9])[A-Za-z]:[\\/]'
            $unixAbsolute = $line -match '(?<![:A-Za-z0-9])/(?:home|Users|opt|mnt|private|tmp|var|srv|root|nas|userhome)/'
            if ($windowsAbsolute -or $unixAbsolute) {
                $violations.Add(("{0}:{1}: machine-specific absolute path" -f $relativePath, $lineNumber))
            }
            foreach ($secretPattern in $secretPatterns) {
                if ($line -match $secretPattern.Pattern) {
                    $violations.Add(
                        ("{0}:{1}: possible {2}" -f
                            $relativePath,
                            $lineNumber,
                            $secretPattern.Name)
                    )
                }
            }
        }
    }

    if ($violations.Count -gt 0) {
        Write-Error ("Repository policy violations:`n{0}" -f ($violations -join "`n"))
    }
    $scope = if ($StagedOnly) { "staged" } else { "working-tree" }
    Write-Host ("Repository policy check passed for {0} {1} files." -f $files.Count, $scope)
}
finally {
    [Console]::OutputEncoding = $previousOutputEncoding
    Pop-Location
}
