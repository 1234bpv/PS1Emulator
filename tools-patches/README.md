# tools-patches — інфраструктура збірки

Без цього збірку **неможливо відтворити** на іншій машині. Docs і configs проєкту локальні (CLAUDE.md §8), тож найкритичніше зібрано тут.

## Вміст

| Файл | Призначення |
|---|---|
| `symbian-env.cmd` | середовище складання: `EPOCROOT`, `PATH`, диск D: |
| `cl_bpabi.pm.patch` | правка SDK: vista-патч без хардкоду шляху до CSL |
| `makdeps.pm.patch` | правка SDK: препроцесор для сканування залежностей |
| `mmp-core/` | `bld.inf` + `pcsxcore.mmp` для ядра (Етап 1) |
| `mmp-gpu/` | `gpuunai.mmp` — gpulib + gpu_unai без асемблера (Етап 2) |
| `mmp-stage0/` | `stage0.mmp` — зонд виконуваної пам'яті (Етап 0) |
| `mmp-stage1/` | `stage1.mmp` — оболонка, що лінкує ядро і GPU |

## Передумови

1. **S60 3rd Edition FP1 SDK** у `D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1`
2. **CSL Arm Toolchain** (GCC 3.4.3, бандл SDK) у `D:\VsCodeProjects\SymbianProjects\toolchain\CSL_Arm_Toolchain`
3. **Strawberry Perl 5.20.x portable** у `D:\VsCodeProjects\SymbianProjects\toolchain\perl`
   Версія критична: `E32Plat.pm:289,387` вживають `defined(%hash)`, що **фатально з Perl 5.22**. Msys-perl із Git Bash не підходить принципово.
4. Форк ядра — `github.com/1234bpv/pcsx_rearmed-symbian`, гілка `symbian`

## Застосування патчів SDK

```cmd
cd D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1\Epoc32\tools
copy cl_bpabi.pm cl_bpabi.pm.orig
copy makdeps.pm  makdeps.pm.orig
patch -u cl_bpabi.pm < <шлях>\cl_bpabi.pm.patch
patch -u makdeps.pm  < <шлях>\makdeps.pm.patch
```

Патчі побудовані як `diff -u <файл>.orig <файл>`, тож накладаються на **штатні** файли SDK.

### Навіщо кожен

**`cl_bpabi.pm`** — застосовано офіційний vista-патч, але з виправленнями. У постачанні патч містить хардкод `C:\Program Files\CSL Arm Toolchain\` у трьох функціях (`GCCInstallPath`, `GCCLibPath`, `GCCRuntimePath`); він спрацьовує, якщо шлях встановлення не містить літерально `CSL Arm Toolchain` (у нас — `CSL_Arm_Toolchain`, з підкресленнями), і збірка ламається. Хардкод прибрано — шлях визначається з самого компілятора. Додатково узгоджено рядки 207-209 із рядком 240: перші очікували корінь, другий — `...\bin`.

**`makdeps.pm`** — дві зміни. (1) Сканування залежностей виконує cpp від CSL 3.4.3: штатний `Epoc32\gcc\bin\cpp.exe` — cygwin-збірка 2003 р., яка падає на варіативних макросах C99 (`libpcsxcore/system.h:51`). (2) Розекранування `\\` у виводі `-M` цього cpp.

`Preprocessor.pm` навмисно **не** чіпається: `prepfile.pm`/`epocrc.pl` викликають cpp із прапорцем `-+`, якого CSL-cpp не приймає.

## Генерований `revision.h`

`libpcsxcore/misc.c:38` включає `"revision.h"`, який upstream генерує Makefile-ом і **ігнорує в git** (`.gitignore:15`). У свіжому клоні його немає → `misc.c:691: parse error before "REV"`. Створити вручну:

```cmd
cd <форк>
git describe --always > tmp && set /p R=<tmp && echo #define REV "%R%" > include\revision.h && del tmp
```
або просто записати рядок на кшталт `#define REV "f8c6f9c"`.

## Збірка ядра

```cmd
call symbian-env.cmd
cd <...>\configs\core\group
bldmake bldfiles
call .\ABLD.BAT build gcce urel
```
Очікуваний результат — `Epoc32\release\armv5\urel\pcsxcore.lib` (~685 КБ, 42 об'єктні файли).

`abld` викликати саме як `call .\ABLD.BAT` — просто `abld` cmd не знаходить.

## Пакування SIS для фізичного апарата

```cmd
call symbian-env.cmd
cd <...>\configs\stage0\group
makesis stage0.pkg ps1stage0.sis
makekeys -cert -password dummy -len 2048 -dname "CN=PS1Emulator-Dev OU=Dev OR=PS1Emulator CO=UA" stage0key.key stage0cert.cer
signsis ps1stage0.sis ps1stage0_signed.sis stage0cert.cer stage0key.key dummy
```

**`.pkg` мусить мати закінчення рядків CRLF і бути в ASCII.** З LF `makesis` мовчки не бачить блок постачальника і падає з `No vendor names found` — повідомлення вказує не на ту причину. Редактори, що зберігають у LF, ламають збірку непомітно.

Ключ і сертифікат (`*.key`, `*.cer`) у git не потрапляють — генеруються локально командою вище. Самопідпис достатній, бо UID у діапазоні `0xE0000000-0xEFFFFFFF` (розробницький) і `CAPABILITY None`.
