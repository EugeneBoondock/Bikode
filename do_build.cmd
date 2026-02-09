@echo off
cd /d "c:\Users\USER\Projects\Bikode"
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" Notepad2e.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:PlatformToolset=v143 /t:Rebuild /m /v:minimal
echo.
echo EXIT_CODE=%ERRORLEVEL%
