@rem Runs format-sources.py from a Windows shell.
@rem
@rem PowerShell and cmd will not execute a .py file given as a bare path -- they hand it to the
@rem .py file association, which opens an editor and looks like nothing happened. This wrapper
@rem exists so 'tools\format-sources.bat --check' does the obvious thing. Same python/py fallback
@rem as waf.bat next to it.
@setlocal
@set PYEXE=python
@where %PYEXE% 1>NUL 2>NUL
@if %ERRORLEVEL% neq 0 set PYEXE=py
@%PYEXE% "%~dp0format-sources.py" %*
@exit /b %ERRORLEVEL%
