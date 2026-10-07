@echo off
rem Drag DuckStation/PSX memory cards and/or PC port saves (N.MCD, or the save folder) onto this file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sh_savecard.ps1" %*
