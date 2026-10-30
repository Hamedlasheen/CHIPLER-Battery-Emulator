@echo off
rem CHIPLER AUTO monitor launcher. RECEIVE ONLY: it never sends anything to the board.
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
  echo Python launcher "py" not found. Install Python 3 from https://www.python.org/ and run this again.
  pause & exit /b 1
)
py -c "import serial" >nul 2>nul
if errorlevel 1 (
  echo pyserial is missing. Install it once with:
  echo     py -m pip install -r requirements.txt
  set /p ANS=Install it now? [y/N] 
  if /i "%ANS%"=="y" ( py -m pip install -r requirements.txt ) else ( pause & exit /b 1 )
)
set /p TRIAL=Trial label (A, B, C ... - NOT the phone model): 
if "%TRIAL%"=="" set TRIAL=A
py chipler_monitor.py --trial %TRIAL%
echo.
echo When you are done with all trials, build the report with:
echo     py chipler_analyze.py
pause
