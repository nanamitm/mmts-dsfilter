@cd /d "%~dp0"
@regsvr32.exe "%~dp0\mmts-dsfilter.ax" /s
@if %errorlevel% NEQ 0 goto error
:success
@echo.
@echo.
@echo    Installation succeeded.
@echo.
@echo    Please do not delete the mmts-dsfilter.ax file.
@echo    The installer has not copied the files anywhere.
@echo    Keep the bundled DLL files and mmts-dsfilter.ini in this folder.
@echo.
@goto done
:error
@echo.
@echo.
@echo    Installation failed.
@echo.
@echo    You need to right click "Install_mmts-dsfilter_64.cmd" and choose "Run as administrator".
@echo.
:done
@pause >NUL