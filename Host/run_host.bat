@echo off
rem ====================================================================
rem  Double-click this file to open the host GUI (live curves).
rem
rem  WHY THE FILENAME AND THIS FILE ARE BOTH ASCII-ONLY:
rem    cmd.exe decodes .bat files using the OEM code page (GBK on this
rem    machine), and command-line arguments get re-encoded on the way in.
rem    A Chinese filename could not even be invoked from Git Bash here
rem    (UTF-8 -> GBK mismatch), and a mangled comment can corrupt the
rem    parsing of the lines after it. This project has already been
rem    burned twice by non-ASCII in tool-consumed files:
rem      - #error text mangled by armcc   (see CLAUDE.md)
rem      - fix-ftype.bat lost all 6 lines (see CLAUDE.md)
rem    Want a Chinese name? Make a DESKTOP SHORTCUT to this file and
rem    rename the shortcut -- shortcuts are Unicode and have no problem.
rem
rem  PORT IS AUTO-DETECTED:
rem    USB serial is preferred, Bluetooth virtual ports are skipped.
rem    To force a port, run in cmd:
rem        run_host.bat --port COM7
rem ====================================================================

chcp 65001 >nul
cd /d "%~dp0"

python monitor_host.py --plot %*
set RC=%ERRORLEVEL%

echo.
if not "%RC%"=="0" echo [exit code %RC%] -- see the messages above.
echo Press any key to close...
pause >nul
