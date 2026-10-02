# Sets up nRF Connect SDK v3.2.1 toolchain environment in the current PowerShell session
$tc = 'C:\ncs\toolchains\66cdf9b75e'
$paths = '.', 'mingw64\bin', 'bin', 'opt\bin', 'opt\bin\Scripts', 'opt\nanopb\generator-bin', 'nrfutil\bin',
         'opt\zephyr-sdk\arm-zephyr-eabi\bin', 'opt\zephyr-sdk\riscv64-zephyr-elf\bin' | ForEach-Object { Join-Path $tc $_ }
$env:PATH = ($paths -join ';') + ';' + $env:PATH
$env:PYTHONPATH = (('opt\bin', 'opt\bin\Lib', 'opt\bin\Lib\site-packages' | ForEach-Object { Join-Path $tc $_ }) -join ';')
$env:NRFUTIL_HOME = Join-Path $tc 'nrfutil\home'
$env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'
$env:ZEPHYR_SDK_INSTALL_DIR = Join-Path $tc 'opt\zephyr-sdk'
$env:ZEPHYR_BASE = 'C:\ncs\v3.2.1\zephyr'
