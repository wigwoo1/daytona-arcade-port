param([Parameter(Mandatory=$true)][string]$SdkRoot,[string]$Picasso='')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(-not $Picasso){$Picasso=Join-Path $SdkRoot 'tools\picasso.exe'}
$output=Join-Path $SdkRoot 'daytona-gpu.shbin'
& $Picasso -o $output (Join-Path $repo '3ds\daytona.v.pica')
if($LASTEXITCODE){throw '3DS shader assembly failed'}
$bytes=[IO.File]::ReadAllBytes($output)
$header='// Generated from 3ds/daytona.v.pica with devkitPro picasso.'+[Environment]::NewLine+'#pragma once'+[Environment]::NewLine+'alignas(4) inline unsigned char daytona_gpu_shader[] = {'+($bytes -join ',')+'};'
[IO.File]::WriteAllText((Join-Path $repo 'src\app\3ds_gpu_shader.h'),$header)
Write-Output "Assembled $($bytes.Length)-byte native 3DS vertex shader."