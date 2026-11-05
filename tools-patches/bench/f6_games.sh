#!/bin/sh
# F6: the five games the C5 matrix never reached, run on the N95. The image is
# chosen by writing game= into ps1_settings.txt, because the img argument cuts
# the name at the first space and eleven of twelve names contain one. The
# original settings file is saved first and put back at the end - losing it is
# a mistake this project already made once.
set -u
# The default is this session's scratchpad and dies with the session; set S in
# the environment to point it somewhere that outlives it.
S="${S:?set S to a scratch folder}"
D=/d/VsCodeProjects/SymbianProjects/SymbianDeployDaemon/tools/deploy_net.ps1
# Transport: "-Ip 192.168.3.22" for the N95 over WiFi, "-Com COM12" for the C5
# over Bluetooth. The games live on the C5 card, so that is the default here.
LINK="${LINK:--Com COM12}"
CFG='E:\PS1\cfg\ps1_settings.txt'
RUNLOG='E:\PS1\logs\ps1_stage1.jsonl'
ALTLOG='E:\PS1\logs\ps1_stage1_alt.jsonl'
mkdir -p "$S/f6"

if [ ! -s "$S/f6/list.txt" ]; then
  echo "ABORT: no $S/f6/list.txt - one line per game, 'tag|filename', filenames"
  echo "       taken from an -Ls of E:\PS1\games, not guessed"
  exit 1
fi

# A bare file name in list.txt costs a whole sweep: the emulator opens no disc,
# logs CDRA_FAIL, and still runs 1200 frames of nothing, so every game reports a
# plausible time and none of them is that game.
if grep -qv '|.*[\]' "$S/f6/list.txt"; then
  echo "ABORT: a line in list.txt names no directory - the disc path must be the"
  echo "       full device path, e.g. 'tag|E:\PS1\games\Name (USA).pbp'"
  grep -nv '|.*[\]' "$S/f6/list.txt"
  exit 1
fi

# Putting the original file back must survive the script being killed, which is
# the way it would actually get lost: the loop below can hang on the daemon.
restore() {
  timeout 300 pwsh -NoProfile -File $D $LINK -PutFile "$S/f6/settings_original.txt" \
    -Remote "$CFG" < /dev/null > /dev/null 2>&1
  timeout 300 pwsh -NoProfile -File $D $LINK -GetFile "$CFG" \
    -OutFile "$S/f6/settings_readback.txt" < /dev/null > /dev/null 2>&1
  if cmp -s "$S/f6/settings_original.txt" "$S/f6/settings_readback.txt"; then
    echo "original settings restored and verified byte for byte"
  else
    echo "WARNING: the settings file on the phone does not match the original -"
    echo "         put $S/f6/settings_original.txt back by hand"
  fi
}

timeout 300 pwsh -NoProfile -File $D $LINK -GetFile "$CFG" \
  -OutFile "$S/f6/settings_original.txt" < /dev/null > /dev/null 2>&1
if [ ! -s "$S/f6/settings_original.txt" ]; then echo "ABORT: no original settings file pulled"; exit 1; fi
echo "original settings saved, $(wc -c < "$S/f6/settings_original.txt") bytes"
trap 'restore; exit 2' INT TERM HUP

n=0
while IFS='|' read -r tag file; do
  [ -z "$tag" ] && continue
  n=$((n+1))
  # NOT sed here: the replacement text is a Windows path, and sed reads its
  # backslashes as escapes - E:\PS1\games\X arrived as E:PS1gamesX, the disc
  # never loaded, and a whole five-game sweep was wasted.
  { grep -v '^game=' "$S/f6/settings_original.txt"
    printf 'game=%s\n' "$file"
  } > "$S/f6/settings_$tag.txt"
  # < /dev/null on every child: otherwise it eats list.txt from the loop's stdin
  # and the sweep silently stops after the first game.
  timeout 300 pwsh -NoProfile -File $D $LINK -PutFile "$S/f6/settings_$tag.txt" \
    -Remote "$CFG" < /dev/null > /dev/null 2>&1
  # Delete the log first. It survives a run that never started - the C5 refuses
  # a harness build with ERR create -46 - and the previous game's log then reads
  # as this one's result.
  # A leftover instance makes the next launch exit at once through the
  # single-instance guard, and the daemon still reports EXIT 0 - from outside
  # that reads exactly like a run that measured something.
  timeout 120 pwsh -NoProfile -File $D $LINK -Cmd "KILL ps1stage1" < /dev/null > /dev/null 2>&1
  timeout 120 pwsh -NoProfile -File $D $LINK -DelFile "$RUNLOG" < /dev/null > /dev/null 2>&1
  timeout 120 pwsh -NoProfile -File $D $LINK -DelFile "$ALTLOG" < /dev/null > /dev/null 2>&1
  timeout 600 pwsh -NoProfile -File $D $LINK -Cmd 'EXEC 180000 E:\sys\bin\ps1stage1.exe gui play nosnd frames1200 light blend dsa' \
    < /dev/null > "$S/f6/run_$tag.txt" 2>&1
  # And remove the local copy too: a failed GetFile leaves the previous game's
  # file sitting there, which is how five identical results once got reported.
  rm -f "$S/f6/log_$tag.jsonl"
  timeout 300 pwsh -NoProfile -File $D $LINK -GetFile "$RUNLOG" \
    -OutFile "$S/f6/log_$tag.jsonl" < /dev/null > /dev/null 2>&1
  # Always try the alternate too, and prefer it. A stuck previous instance keeps
  # the primary file in place AND locked, so the primary pull succeeds and hands
  # back that instance's stale content - which is exactly the trap this guards.
  rm -f "$S/f6/alt_$tag.jsonl"
  timeout 300 pwsh -NoProfile -File $D $LINK -GetFile "$ALTLOG" \
    -OutFile "$S/f6/alt_$tag.jsonl" < /dev/null > /dev/null 2>&1
  if [ -s "$S/f6/alt_$tag.jsonl" ]; then
    mv -f "$S/f6/alt_$tag.jsonl" "$S/f6/log_$tag.jsonl"
  fi
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
