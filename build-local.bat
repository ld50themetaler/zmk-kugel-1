@echo off
setlocal
echo Running ZMK build on WSL Docker...

:: Execute build-local.sh via WSL using wslpath or $HOME fallback
wsl bash -c "DIR=$(wslpath '%~dp0' 2>/dev/null); if [ -n \"$DIR\" ] && [ -f \"$DIR/build-local.sh\" ]; then bash \"$DIR/build-local.sh\"; elif [ -f \"$HOME/zmk-kugel-1/build-local.sh\" ]; then bash \"$HOME/zmk-kugel-1/build-local.sh\"; else echo '[ERROR] Could not find build-local.sh in WSL!'; exit 1; fi"

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Build failed or script not found.
)

pause
