#!/bin/sh
# Stop BBM's EBMService and start ours (/var/rootdata/wacalls-ebm). Undo: restore.sh or reboot.
export PATH=/accounts/1000/shared/misc/berrycore/bin:$PATH LD_LIBRARY_PATH=/accounts/1000/shared/misc/berrycore/bin:/accounts/1000/shared/misc/berrycore/lib
D=/var/rootdata/wacalls-ebm
for p in $(pidin -f "a n" ar | grep 'app/native/ebmservice.conf' | grep -v grep | awk '{print $1}'); do
    echo "stopping BBM EBMService $p"; kill $p
done
for p in $(pidin -f "a n" ar | grep 'EBMService.wa' | grep -v grep | awk '{print $1}'); do
    echo "stopping our previous EBMService $p"; kill $p
done
sleep 2
nohup $D/run.sh > $D/out.log 2>&1 < /dev/null &
sleep 3
pidin -f "a n" ar | grep -i ebm | grep -v grep
