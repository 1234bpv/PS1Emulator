# F11 evidence, 2026-10-05

Canonical report: ../f11-game-speed-2026-10-05.md.
15 valid 1200-frame runs: C5 GT/Crash and N95-stock GT/Crash/WipEout.
Use the 15 *.result.json and matching complete *.jsonl; older smoke,
menu/navigation, partial transfers and failed transport attempts are evidence,
not performance results. summary-progress.json has five completed groups.

Accepted gameplay states are exactly those in states/manifest.json:
gt-race.sav, crash-start.sav, wipeout-driven2.sav.
The navigation/ SAV files are menu/intro candidates for later continuation.
No Castlevania gameplay state was fully downloaded. Partial SAV files remain
in the original run folder, separately named, and are never accepted states.

resume-restoration-verification.json is the CURRENT restoration audit:
31 identical=true, 1 unverified C5 BENCH.SAV. PUT and STAT succeeded for that
file; three GET attempts timed out. The older restoration-verification.json
only concerns restoration BEFORE this continued test session.
The current absence proofs are individual final *-stat.stdout.txt files.
N95-final-stock-clock-retry.log is the final complete register proof.

Raw stdout/stderr files use Windows PowerShell console encoding CP866;
*.utf8.txt copies decode that encoding for reading. Emulator JSONL is UTF-8.
BMP files are raw phone screenshots. Referenced PNGs are format conversions;
C5 portrait/cropping is preserved. Phone times differ from PC wall time.

Scripts are review copies. The working originals, original backups and full
raw files are in PS1Emulator/plans/runs/2026-10-05_game-speed/.
Original emulator source and deploy_net.ps1 were not changed. The local
restoration-client.ps1 copy only raises SerialPort.ReadBufferSize to 1 MiB;
that did not fix the incomplete C5 GET. No further test was run after the
third failed Castlevania state transfer.
