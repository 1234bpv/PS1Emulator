#!/bin/sh
# F0/F5: which compiler builds the core. Same source, same clock (configuration
# A, set and read back by ocprobe), three runs each, interleaved so a drift in
# the phone cannot be mistaken for a difference between compilers.
set -u
# The default is this session's scratchpad and dies with the session; set S in
# the environment to point at wherever the two EXEs actually live.
S="${S:-C:/Users/7931~1/AppData/Local/Temp/claude/D--VsCodeProjects-SymbianProjects-PS1Emulator/f4f7c4be-10eb-42f7-826f-af82b9a69b11/scratchpad}"
D=/d/VsCodeProjects/SymbianProjects/SymbianDeployDaemon/tools/deploy_net.ps1
IP=192.168.3.22
REMOTE='E:\sys\bin\ps1stage1.exe'
OCLOG='E:\ocprobe.log'
RUNLOG='E:\PS1\logs\ps1_stage1.jsonl'
# No dsa in the line: on the N95 the direct-screen path currently kills the run
# at frame 7702 with EXIT -1059 WSERV (see work/state.md, belongs to F9). Both
# compilers are measured on the window path, which makes them comparable with
# each other - the only thing this experiment asks.
CMD='EXEC 300000 E:\sys\bin\ocprobe.exe patcher clocks mult=86 mpu=2 l3=4 dss=6 sec=150 child=ps1 args:gui play nosnd frames8300 light blend loadstate7700 imggt.pbp'

for cc in csl343 gcce463; do
  if [ ! -s "$S/cc/ps1stage1-$cc.exe" ]; then
    echo "ABORT: $S/cc/ps1stage1-$cc.exe is missing - build both flavours first"
    exit 1
  fi
done

for round in 1 2 3; do
  for cc in csl343 gcce463; do
    timeout 300 pwsh -NoProfile -File $D -Ip $IP -PutFile "$S/cc/ps1stage1-$cc.exe" -Remote "$REMOTE" > "$S/cc/push_${cc}_$round.txt" 2>&1
    if [ $? -ne 0 ]; then echo "$cc r$round: PUSH FAILED"; continue; fi
    # Delete the log before the run, not after. It survives a failed run, and a
    # stale drc: total= line from the PREVIOUS compiler reads as a valid result
    # for this one - a wrong number, which is worse than no number.
    timeout 120 pwsh -NoProfile -File $D -Ip $IP -DelFile "$RUNLOG" > /dev/null 2>&1
    timeout 900 pwsh -NoProfile -File $D -Ip $IP -Cmd "$CMD" > "$S/cc/run_${cc}_$round.txt" 2>&1
    rm -f "$S/cc/log_${cc}_$round.jsonl" "$S/cc/oc_${cc}_$round.log"
    timeout 300 pwsh -NoProfile -File $D -Ip $IP -GetFile "$RUNLOG" -OutFile "$S/cc/log_${cc}_$round.jsonl" > /dev/null 2>&1
    timeout 300 pwsh -NoProfile -File $D -Ip $IP -GetFile "$OCLOG" -OutFile "$S/cc/oc_${cc}_$round.log" > /dev/null 2>&1
    if [ ! -s "$S/cc/log_${cc}_$round.jsonl" ]; then
      echo "$cc r$round: NO LOG - the run did not produce one, this round is void"
      continue
    fi
    perf=$(grep -o 'drc: total=[^"]*' "$S/cc/log_${cc}_$round.jsonl" 2>/dev/null | tail -1)
    dirty=$(grep -c 'NOT STOCK AT START\|MISMATCH' "$S/cc/oc_${cc}_$round.log" 2>/dev/null)
    echo "$cc r$round dirty=$dirty ${perf:-NO PERF LINE}"
  done
done
