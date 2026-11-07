# 对每个 cow 进程分别统计 Private_Dirty(只匹配一行)
for pid in $(pgrep cow); do
  echo -n "pid=$pid Private_Dirty(KB): "
  grep "Private_Dirty" /proc/$pid/smaps | awk '{s+=$2} END {print s}'
done