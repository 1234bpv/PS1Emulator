#!/bin/sh
# F6: the five games the C5 matrix never reached, run on the N95. The image is
# chosen by writing game= into ps1_settings.txt, because the img argument cuts
# the name at the first space and eleven of twelve names contain one. The
# original settings file is saved first and put back at the end - losing it is
# a mistake this project already made once.
set -u
# The default is this session's scratchpad and dies with the session; set S in
# the environment to point it somewhere that outlives it.
S="${S:-C:/Users/7931~1/AppData/Local/Temp/claude/D--VsCodeProjects-SymbianProjects-PS1Emulator/f4f7c4be-10eb-42f7-826f-af82b9a69b11/scratchpad}"
D=/d/VsCodeProjects/SymbianProjects/SymbianDeployDaemon/tools/deploy_net.ps1
IP=192.168.3.22
CFG='E:\PS1\cfg\ps1_settings.txt'
RUNLOG='E:\PS1\logs\ps1_stage1.jsonl'
mkdir -p "$S/f6"

if [ ! -s "$S/f6/list.txt" ]; then
  echo "ABORT: no $S/f6/list.txt - one line per game, 'tag|filename', filenames"
  echo "       taken from an -Ls of E:\PS1\games, not guessed"
  exit 1
fi

# Putting the original file back must survive the script being killed, which is
# the way it would actually get lost: the loop below can hang on the daemon.
restore() {
  timeout 300 pwsh -NoProfile -File $D -Ip $IP -PutFile "$S/f6/settings_original.txt" \
    -Remote "$CFG" < /dev/null > /dev/null 2>&1
  timeout 300 pwsh -NoProfile -File $D -Ip $IP -GetFile "$CFG" \
    -OutFile "$S/f6/settings_readback.txt" < /dev/null > /dev/null 2>&1
  if cmp -s "$S/f6/settings_original.txt" "$S/f6/settings_readback.txt"; then
    echo "original settings restored and verified byte for byte"
  else
    echo "WARNING: the settings file on the phone does not match the original -"
    echo "         put $S/f6/settings_original.txt back by hand"
  fi
}

timeout 300 pwsh -NoProfile -File $D -Ip $IP -GetFile "$CFG" \
  -OutFile "$S/f6/settings_original.txt" < /dev/null > /dev/null 2>&1
if [ ! -s "$S/f6/settings_original.txt" ]; then echo "ABORT: no original settings file pulled"; exit 1; fi
echo "original settings saved, $(wc -c < "$S/f6/settings_original.txt") bytes"
trap 'restore; exit 2' INT TERM HUP

n=0
while IFS='|' read -r tag file; do
  [ -z "$tag" ] && continue
  n=$((n+1))
  sed "s|^game=.*|game=$file|" "$S/f6/settings_original.txt" > "$S/f6/settings_$tag.txt"
  grep -q "^game=" "$S/f6/settings_$tag.txt" || printf 'game=%s\n' "$file" >> "$S/f6/settings_$tag.txt"
  # < /dev/null on every child: otherwise it eats list.txt from the loop's stdin
  # and the sweep silently stops after the first game.
  timeout 300 pwsh -NoProfile -File $D -Ip $IP -PutFile "$S/f6/settings_$tag.txt" \
    -Remote "$CFG" < /dev/null > /dev/null 2>&1
  timeout 600 pwsh -NoProfile -File $D -Ip $IP -Cmd 'EXEC 180000 E:\sys\bin\ps1stage1.exe gui play nosnd frames1200 light blend dsa' \
    < /dev/null > "$S/f6/run_$tag.txt" 2>&1
  timeout 300 pwsh -NoProfile -File $D -Ip $IP -GetFile "$RUNLOG" \
    -OutFile "$S/f6/log_$tag.jsonl" < /dev/null > /dev/null 2>&1
  if [ ! -s "$S/f6/log_$tag.jsonl" ]; then
    echo "$tag NO LOG PULLED - treat as a failed run, not as a silent pass"
    continue
  fi
  disc=$(grep -o 'Loaded CD Image: [^"]*' "$S/f6/log_$tag.jsonl" 2>/dev/null | tail -1)
  shut=$(grep -o '"msg":"gui: frames=[^"]*' "$S/f6/log_$tag.jsonl" 2>/dev/null | tail -1)
  err=$(grep -c '"error":"[A-Z]' "$S/f6/log_$tag.jsonl" 2>/dev/null)
  echo "$tag errors=$err | ${disc:-NO DISC LINE} | ${shut:-NO SHUTDOWN LINE}"
done < "$S/f6/list.txt"

echo "$n games attempted"
trap - INT TERM HUP
restore
