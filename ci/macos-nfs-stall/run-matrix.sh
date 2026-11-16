#!/bin/zsh
# Run cvrepro configurations concurrently; their combined load also
# oversubscribes the CPUs. Logs: out/<name>.log, exit code in out/<name>.rc.
D=${1:-7200}
cd ${0:A:h}; mkdir -p out
typeset -A cfg
cfg=(
  base     ""
  preempt  "--hogs 4 --worker-qos background"
  fork     "--fork-ms 2 --spawn-ms 5"
  signals  "--signal-us 100"
  suspend  "--suspend-ms 2"
  all      "--hogs 2 --worker-qos utility --fork-ms 5 --spawn-ms 10 --signal-us 500 --suspend-ms 10"
)
for n in ${(k)cfg}; do
  ( ./cvrepro --duration-s $D --stall-ms 5000 ${=cfg[$n]} > out/$n.log 2>&1; echo $? > out/$n.rc ) &
done
wait
