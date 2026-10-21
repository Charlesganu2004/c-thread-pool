$ErrorActionPreference = "Stop"

gcc -std=c11 -Wall -Wextra -Wpedantic -O2 `
    -D_WIN32_WINNT=0x0600 `
    -Iinclude `
    src\main.c src\thread_pool.c `
    -o thread_pool_demo.exe

if ($LASTEXITCODE -ne 0) {
    Write-Error "gcc failed with exit code $LASTEXITCODE" -ErrorAction Continue
    exit $LASTEXITCODE
}

Write-Host "Built thread_pool_demo.exe"
