sc create cimoddrv binPath=%cd%\cimoddrv.sys type=kernel start=demand DisplayName=cimoddrv error=ignore
sc stop cimoddrv

cdb.exe -z c:\windows\system32\ci.dll -cf ci.sc > ci.txt
for /f "tokens=5" %%A in ('findstr /c:Evaluate ci.txt') DO SET CIOFFSET=0x%%A
for /f "tokens=1" %%A in ('findstr /c:stamp ci.txt') DO SET CITS=0x%%A
del ci.txt
reg add HKLM\System\CurrentControlSet\Services\cimoddrv /v g_CiOptions /t REG_DWORD /d %CIOFFSET% /f
reg add HKLM\System\CurrentControlSet\Services\cimoddrv /v cidll_ts /t REG_DWORD /d %CITS% /f

cdb.exe -z c:\windows\system32\ntoskrnl.exe -cf ntos.sc > ntos.txt
for /f "tokens=5" %%A in ('findstr /c:Evaluate ntos.txt') DO SET NTOFFSET=0x%%A
for /f "tokens=1" %%A in ('findstr /c:stamp ntos.txt') DO SET NTTS=0x%%A
del ntos.txt
reg add HKLM\System\CurrentControlSet\Services\cimoddrv /v SeILSigningPolicy /t REG_DWORD /d %NTOFFSET% /f
reg add HKLM\System\CurrentControlSet\Services\cimoddrv /v ntos_ts /t REG_DWORD /d %NTTS% /f

sc start cimoddrv

cimod.exe

