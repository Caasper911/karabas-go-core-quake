#!/bin/bash
# Parallel MAP / PAR of the Mini G with several cost tables (karabas-go-core-quake Ф3б), each in its own copy
# on D: (the XST netlist is reused). A watchdog kills a PAR that has not written its log for 15 minutes.
# usage: [BOARD=go|mini|minig] par_multi.sh 4 5 6   (default minig)
R=/c/hobby/karabas-go-core-quake
BD=${BOARD:-minig}
B=/d/.claude/karabas-go-core-quake/par/$BD
for t in "$@"; do
  d=$B/t$t
  rm -rf $d; mkdir -p $d/src
  cp -rp $R/modules $d/
  cd $R/src
  cp -p Makefile karabas_$BD.prj karabas_$BD.xst karabas_$BD.ut karabas_$BD.ucf karabas_$BD.ngc core.json $d/src/
  cp -rp rtl $d/src/
  touch $d/src/karabas_$BD.ngc
  ( cd $d/src && make $BD-impl MAP_T_$BD=$t > ../build.log 2>&1; echo "DONE $?" >> ../build.log ) &
done
# watchdog
while true; do
  sleep 60
  left=0
  for t in "$@"; do
    l=$B/t$t/build.log
    grep -q "^DONE" $l 2>/dev/null && continue
    left=1
    age=$(( $(date +%s) - $(stat -c %Y $l) ))
    if [ $age -gt 900 ]; then
      echo "t$t: no progress for $age s, killing its par"
      for pid in $(ps -W | grep -i 'par.exe' | awk '{print $4}'); do
        wmic process where "ProcessId=$pid" get CommandLine 2>/dev/null | grep -q "t$t" && taskkill //F //PID $pid > /dev/null
      done
      touch $l
    fi
  done
  [ $left = 0 ] && break
done
for t in "$@"; do
  d=$B/t$t/src
  echo "== t$t: $(grep -m1 -E 'All constraints were met|timing errors|constraints not met' $d/karabas_$BD.twr 2>/dev/null)"
  sed -n "/TS_pll_clkout_0_ = PERIOD/,\$p" $d/karabas_$BD.twr 2>/dev/null | grep -m1 "Minimum period is"
  grep -E "Timing Score" $B/t$t/build.log | tail -1
done
