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

## Після зміни ядра EXE обов'язково перезбирати з `reallyclean`

`abld` відстежує лише власні вихідні файли цілі. Зміна `pcsxcore.lib` чи `gpuunai.lib` перелінкування **не викликає**: `abld build` для `stage1` мовчки нічого не робить, і EXE лишається зі старою бібліотекою всередині.

```cmd
cd <...>\configs\core\group  & call .\ABLD.BAT build gcce urel
cd <...>\configs\stage1\group & call .\ABLD.BAT reallyclean gcce urel & bldmake bldfiles & call .\ABLD.BAT build gcce urel
```

Перевірка одна: **час зміни `ps1stage1.exe` мусить бути пізнішим за час `pcsxcore.lib`.** Інакше на пристрій поїде збірка без ваших правок, і налагодження піде хибним шляхом — виправлення виглядатиме так, ніби воно не подіяло.

## Пакування SIS для фізичного апарата

```cmd
call symbian-env.cmd
cd <...>\configs\stage0\group
makesis stage0.pkg ps1stage0.sis
openssl genrsa -out sha1.key 1024
openssl req -new -x509 -sha1 -key sha1.key -out sha1.cer -days 3650 -subj "/CN=PS1Emulator Dev"
signsis -s ps1stage0.sis ps1stage0_sha1.sis sha1.cer sha1.key
signsis -o ps1stage0_sha1.sis
```

**Сертифікат мусить бути RSA + SHA-1, і робить його openssl, не `makekeys`.** Це перевірено на Nokia N95 8GB (S60v3 FP1, Symbian 9.2, ПЗ v35): SHA-1 встановлюється, SHA-256 дає **«файл пошкоджено»**.

Symbian 9.2 — 2006 рік, його SWInstall не знає `sha256WithRSAEncryption`. Сертифікат не розбирається взагалі, тож інсталятор не доходить навіть до перевірки строку дії й повідомляє про пошкодження файлу. Помилка вказує на вміст пакета, хоча несправний лише сертифікат — на цю хибну підказку легко втратити багато часу.

Діагностична ознака, що відрізняє два випадки:
- **«файл пошкоджено»** — сертифікат не розібрано (SHA-256, або DSA від `makekeys`);
- **«прострочений сертифікат»** — сертифікат розібрано, апарат дійшов до дати. Так поводиться оригінальний PIPS 1.7, підписаний Nokia через Symbian CA до 05.08.2020.

Еталон для звірки — сертифікат Ensymble (`ensymble/utils/defaultcert.py`, zlib+base64): RSA-1024, `sha1WithRSAEncryption`, 2006-2036. Підписаний ним пакет ставиться, тож при сумнівах ним перевіряють, чи справа в сертифікаті, чи в самому SIS.

`makekeys` із цього SDK не годиться принципово: він робить лише DSA (RSA задати нічим), а на DSA покладатися не варто. Приклад у його ж довідці радить 2048 біт — він розрахований на новішу версію з RSA.

**`-s` обов'язковий.** Без нього `signsis` мовчки не підписує і код повернення нульовий — виглядає як успіх. Перевірка одна: `signsis -o` має надрукувати `Issued by`/`Issued to`; порожній вивід означає непідписаний файл.

**`.pkg` мусить мати закінчення рядків CRLF і бути в ASCII.** З LF `makesis` мовчки не бачить блок постачальника і падає з `No vendor names found` — повідомлення вказує не на ту причину. Редактори, що зберігають у LF, ламають збірку непомітно.

Ключ і сертифікат (`*.key`, `*.cer`) у git не потрапляють — генеруються локально командою вище. Самопідпис достатній, бо UID у діапазоні `0xE0000000-0xEFFFFFFF` (розробницький) і `CAPABILITY None`.
