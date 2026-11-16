@echo off
setlocal
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
if not defined RPCS3_DIR set "RPCS3_DIR=E:\RPCS3"

set "PS3_VFS_ROOT=%ROOT%\data\dev_bdvd"
set "PS3_HDD0_ROOT=%ROOT%\data\dev_hdd0"
set "PS3_HDD1_ROOT=%ROOT%\data\dev_hdd1"
set "PS3_GAMEDATA_ROOT=%ROOT%\data"
if not defined PS3_DEV_FLASH set "PS3_DEV_FLASH=%RPCS3_DIR%\dev_flash"
if not defined PS3_VERBOSE set "PS3_VERBOSE=0"

"%~dp0build\AsuraRecomp.exe" "%PS3_VFS_ROOT%\PS3_GAME\USRDIR\EBOOT.elf" %*
exit /b %ERRORLEVEL%
