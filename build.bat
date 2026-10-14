@echo off
rem ---------------------------------------------------------------------------
rem  Builds the emulator and its unit tests with MSVC.
rem
rem    build.bat              release build, then runs the unit tests
rem    build.bat debug        unoptimised build
rem
rem  From a Developer Command Prompt for Visual Studio.  To use CMake and
rem  ctest directly instead:
rem
rem    cmake -S . -B build
rem    cmake --build build --config Release
rem    ctest --test-dir build -C Release --output-on-failure
rem ---------------------------------------------------------------------------

setlocal

where cl >nul 2>&1
if errorlevel 1 (
    echo.
    echo   MSVC was not found on this machine.
    echo.
    echo   Open a "Developer Command Prompt for VS", or run this from an
    echo   x64 Native Tools prompt, and try again.
    echo.
    echo   Alternatively install MinGW-w64 and run:  mingw32-make
    echo.
    exit /b 1
)

set CONFIG=Release
if /I "%~1"=="debug" set CONFIG=Debug

set ROOT=%~dp0
set BUILD=%ROOT%build-msvc

where cmake >nul 2>&1
if errorlevel 1 (
    echo.
    echo   CMake was not found.  Install it, or run:
    echo.
    echo     nmake /f Makefile
    echo.
    exit /b 1
)

echo Building %CONFIG%...
cmake -S "%ROOT%" -B "%BUILD%" >nul || exit /b 1
cmake --build "%BUILD%" --config %CONFIG% || exit /b 1

set EXE=%BUILD%%CONFIG%\gbcpu.exe
set TESTS=%BUILD%%CONFIG%\gbtests.exe

echo.
echo Running the unit tests...
"%TESTS%"
if errorlevel 1 (
    echo.
    echo TESTS FAILED
    exit /b 1
)

echo.
echo All good.  Run it with:
echo   %EXE% yourgame.gb
endlocal