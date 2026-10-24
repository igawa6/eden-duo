#!/bin/bash
# Read-only recorder for an emulator process on the AYN Thor: memory, GPU load/clock/temp, battery.
# Usage: bash record_engine.sh <process name> <out.tsv> [seconds=3600]
#   e.g. bash record_engine.sh org.azahar_emu.azahar.fatesfix:engine_switch ff-mem.tsv
# One line per ~5 s. Missing values print as "-" (adb hiccups never stop it).
export MSYS_NO_PATHCONV=1
proc=$1; out=$2; dur=${3:-3600}
[ -z "$proc" ] || [ -z "$out" ] && { echo "usage: $0 <process> <out.tsv> [seconds]"; exit 2; }
echo -e "time\tpid\trss_mb\tswap_mb\tavail_mb\tgpu_busy\tgpu_mhz\tgpu_c\tbattery" > "$out"
end=$((SECONDS + dur))
while [ $SECONDS -lt $end ]; do
  line=$(adb shell "p=\$(pidof $proc); a=\$(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2); \
g=\$(tr -d ' %' < /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage); c=\$(cat /sys/class/kgsl/kgsl-3d0/gpuclk); \
t=\$(cat /sys/class/kgsl/kgsl-3d0/temp); b=\$(cat /sys/class/power_supply/battery/capacity); r=-; w=-; \
if [ -n \"\$p\" ]; then r=\$(grep VmRSS /proc/\$p/status | tr -s ' ' | cut -d' ' -f2); \
w=\$(grep VmSwap /proc/\$p/status | tr -s ' ' | cut -d' ' -f2); fi; echo \${p:--} \$r \$w \$a \$g \$c \$t \$b" 2>/dev/null | tr -d '\r')
  set -- $line
  mb() { [[ "$1" =~ ^[0-9]+$ ]] && echo $(( $1 / 1024 )) || echo -; }
  mhz() { [[ "$1" =~ ^[0-9]+$ ]] && echo $(( $1 / 1000000 )) || echo -; }
  deg() { [[ "$1" =~ ^[0-9]+$ ]] && echo $(( $1 / 1000 )) || echo -; }
  echo -e "$(date +%H:%M:%S)\t${1:--}\t$(mb "$2")\t$(mb "$3")\t$(mb "$4")\t${5:--}\t$(mhz "$6")\t$(deg "$7")\t${8:--}" >> "$out"
  sleep 5
done
