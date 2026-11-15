@echo off
rem Builds XPatch360.xex. Run it from the "Xbox 360 Command Prompt" (Xbox 360 SDK),
rem in this folder. Set XEXTOOL to where your xextool.exe is.

set XEXTOOL=C:\Tools\xextool\xextool.exe

if "%XEDK%"=="" (
    echo XEDK is not set. Run this from the Xbox 360 Command Prompt.
    exit /b 1
)
if exist "%XEXTOOL%\" (
    echo XEXTOOL points to a folder: %XEXTOOL%
    echo It must point to the program itself, e.g. ...\xextool.exe
    exit /b 1
)
if not exist "%XEXTOOL%" (
    echo xextool not found: %XEXTOOL%
    echo Edit the XEXTOOL line at the top of build.bat.
    exit /b 1
)

cl /nologo /c /O2 /GS- /EHsc /D NDEBUG /D _XBOX /I "%XEDK%\include\xbox" main.cpp || exit /b 1

rem /DLL + <sysdll/>     -> system DLL (Dashlaunch plugin), not a title
rem /ALIGN:128,4096      -> 4 KB pages, required for the 0x9xxxxxxx range
rem /BASE:0x91D00000     -> the loader never relocates; keep clear of other plugins
link /nologo /DLL /ALIGN:128,4096 /BASE:0x91D00000 /XEXCONFIG:xex.xml /OUT:XPatch360.dll /LIBPATH:"%XEDK%\lib\xbox" main.obj xapilib.lib xboxkrnl.lib || exit /b 1

rem The linker writes the XEX under the /OUT name; the PE goes to XPatch360.pe
copy /Y XPatch360.dll XPatch360.xex >nul

rem Devkit -> retail, unencrypted, no limits (needed on RGH/JTAG)
"%XEXTOOL%" -m r -r a -e u XPatch360.xex || exit /b 1

echo.
echo Done: XPatch360.xex
