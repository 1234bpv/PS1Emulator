#!/bin/sh
# F6: the five games the C5 matrix never reached, run on the N95. The image is
# chosen by writing game= into ps1_settings.txt, because the img argument cuts
# the name at the first space and eleven of twelve names contain one. The
# original settings file is saved first and put back at the end - losing it is
# a mistake this project already made once.
set -u
S="C:/Users/7931~1/AppData/Local/Temp/claude/D--VsCodeProjects-SymbianProjects-PS1Emulator/f4f7c4be-10eb-42f7-826f-af82b9a69b11/scratchpad"
D=/d/VsCodeProjects/SymbianProjects/SymbianDeployDaemon/tools/deploy_net.ps1
IP=192.168.3.22
CFG='E:\PS1\cfg\ps1_settings.txt'
RUNLOG='E:\PS1\logs\ps1_stage1.jsonl'
mkdir -p "$S/f6"

timeout 300 pwsh -NoProfile -File $D -Ip $IP -GetFile "$CFG" -OutFile "$S/f6/settings_original.txt" > /dev/null 2>&1
if [ ! -s "$S/f6/settings_original.txt" ]; then echo "ABORT: no original settings file pulled"; exit 1; fi
echo "original settings saved, $(wc -c < "$S/f6/settings_original.txt") bytes"

n=0
while IFS='|' read -r tag file; do
  [ -z "$tag" ] && continue
  n=$((n+1))
  sed "s|^game=.*|game=$file|" "$S/f6/settings_original.txt" > "$S/f6/settings_$tag.txt"
  grep -q "^game=" "$S/f6/settings_$tag.txt" || printf 'game=%s\n' "$file" >> "$S/f6/settings_$tag.txt"
  timeout 300 pwsh -NoProfile -File $D -Ip $IP -PutFile "$S/f6/settings_$tag.txt" -Remote "$CFG" > /dev/null 2>&1
  timeout 600 pwsh -NoProfile -File $D -Ip $IP -Cmd 'EXEC 180000 E:\sys\bin\ps1stage1.exe gui play nosnd frames1200 light blend dsa' > "$S/f6/run_$tag.txt" 2>&1
  timeout 300 pwsh -NoProfile -File $D -Ip $IP -GetFile "$RUNLOG" -OutFile "$S/f6/log_$tag.jsonl" > /dev/null 2>&1
  disc=$(grep -o 'Loaded CD Image: [^"]*' "$S/f6/log_$tag.jsonl" 2>/dev/null | tail -1)
  shut=$(grep -o '"msg":"gui: frames=[^"]*' "$S/f6/log_$tag.jsonl" 2>/dev/null | tail -1)
  err=$(grep -c '"error":"[A-Z]' "$S/f6/log_$tag.jsonl" 2>/dev/null)
  echo "$tag errors=$err | ${disc:-NO DISC LINE} | ${shut:-NO SHUTDOWN LINE}"
done < "$S/f6/list.txt"

timeout 300 pwsh -NoProfile -File $D -Ip $IP -PutFile "$S/f6/settings_original.txt" -Remote "$CFG" > /dev/null 2>&1
echo "original settings restored"
