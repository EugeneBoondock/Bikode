@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul 2>&1
msbuild c:\Users\USER\Projects\Bikode\Bikode.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal > c:\Users\USER\Projects\Bikode\build_output.log 2>&1
echo EXIT_CODE=%ERRORLEVEL% >> c:\Users\USER\Projects\Bikode\build_output.log
