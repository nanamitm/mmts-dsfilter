@cd /d "%~dp0"
@regsvr32.exe "%~dp0\mmts-dsfilter.ax" /u /s
@if %errorlevel% NEQ 0 goto error
:success
@echo.
@echo.
@echo    Uninstallation succeeded.
@echo.
@goto done
:error
@echo.
@echo.
@echo    Uninstallation failed.
@echo.
@echo    You need to right click "Uninstall_mmts-dsfilter_64.cmd" and choose "Run as administrator".
@echo.
:done
@pause >NUL