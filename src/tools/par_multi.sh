#!/bin/bash
# Parallel MAP / PAR of a board with several cost tables, each in its own copy on D: (the XST netlist .ngc of
# src/ is reused). The first run that meets the timing and has its .bin wins: the other runs are stopped at once
# and the script ends (a slow or hung run never holds the next board). A run is stopped when its log is silent
# for SILENT seconds (a PAR hung in phase 4) or after LIMIT seconds. At most 2 runs at a time is the advice:
# three MAPs at once crashed (segmentation fault) at 91 % slices.
# usage: [BOARD=go|mini|minig] [LIMIT=2100] [SILENT=900] [MAKEARGS=...] par_multi.sh 4 5 6    (default minig)
# MAKEARGS: extra make arguments of each run (e.g. MAKEARGS="MAP_MT=off")
# out: the winner's files are in /d/.claude/karabas-go-core-quake/par/<board>/t<N>/src; "WIN t<N>" is printed.
R=/c/hobby/karabas-go-core-quake
BD=${BOARD:-minig}
B=/d/.claude/karabas-go-core-quake/par/$BD
LIMIT=${LIMIT:-2100}
SILENT=${SILENT:-900}
declare -A pid start

kill_run() {   # the Windows process tree of the run t$1 (make -> xst / map / par / bitgen)
  local p=${pid[$1]}
  [ -n "$p" ] || return
  local w=$(cat /proc/$p/winpid 2>/dev/null)
  [ -n "$w" ] && taskkill //F //T //PID $w > /dev/null 2>&1
  kill $p 2> /dev/null
  echo "DONE killed" >> $B/t$1/build.log
}

met() {   # the run t$1 ended with its .bin and no timing error
  local d=$B/t$1/src
  grep -q "^DONE 0" $B/t$1/build.log 2>/dev/null && [ -s $d/karabas_$BD.bin ] &&
    grep -q "Timing Score: 0 " $B/t$1/build.log 2>/dev/null
}

for t in "$@"; do
  d=$B/t$t
  rm -rf $d; mkdir -p $d/src
  cp -rp $R/modules $d/
  cd $R/src
  cp -p Makefile karabas_$BD.prj karabas_$BD.xst karabas_$BD.ut karabas_$BD.ucf karabas_$BD.ngc core.json $d/src/
  cp -rp rtl $d/src/
  touch $d/src/karabas_$BD.ngc
  ( cd $d/src && make $BD-impl MAP_T_$BD=$t $MAKEARGS > ../build.log 2>&1; echo "DONE $?" >> ../build.log ) &
  pid[$t]=$!
  start[$t]=$(date +%s)
done

win=""
while true; do
  sleep 30
  left=0
  for t in "$@"; do
    l=$B/t$t/build.log
    grep -q "^DONE" $l 2>/dev/null && { met $t && [ -z "$win" ] && win=$t; continue; }
    left=1
    now=$(date +%s)
    if [ $(( now - $(stat -c %Y $l) )) -gt $SILENT ]; then echo "t$t: silent for $SILENT s, stopped"; kill_run $t; fi
    if [ $(( now - ${start[$t]} )) -gt $LIMIT ]; then echo "t$t: over $LIMIT s, stopped"; kill_run $t; fi
  done
  if [ -n "$win" ]; then
    for t in "$@"; do grep -q "^DONE" $B/t$t/build.log 2>/dev/null || { echo "t$t: stopped (t$win won)"; kill_run $t; }; done
    break
  fi
  [ $left = 0 ] && break
done

for t in "$@"; do
  d=$B/t$t/src
  echo "== t$t: $(grep -m1 -E 'All constraints were met|timing errors|constraints not met' $d/karabas_$BD.twr 2>/dev/null) $(grep "Timing Score" $B/t$t/build.log 2>/dev/null | tail -1)"
  sed -n "/TS_pll_clkout_0_ = PERIOD/,\$p" $d/karabas_$BD.twr 2>/dev/null | grep -m1 "Minimum period is"
done
[ -n "$win" ] && echo "WIN t$win" || echo "no run met the timing"
