@echo off
cd /d "%~dp0src\mf_source"
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" fluxmic_mf_source.vcxproj /p:Configuration=Release /p:Platform=x64 /p:OutDir=%~dp0build\
