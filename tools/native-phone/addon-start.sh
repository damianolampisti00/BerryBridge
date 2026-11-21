#!/bin/sh
# AutoRoot V4 boot addon: /var/addons/wacallsebm/init/start.sh (with an empty
# /var/addons/wacallsebm/data next to init/). btool runs it with /bin/sh as
# root ~8 min after power-on.
# Why: WhatsApp calls reach the native Phone app only while OUR copy of the
# BBM Video engine (/var/rootdata/wacalls-ebm/EBMService.wa + dialhelper) is
# registered to the WaCalls gateway. After a reboot only BBM's own EBMService
# starts (no BBID: never registers) and calls fall back to the Berry Bridge
# audio path (2026-11-03 07:17 and 15:20 boots). This runs swap.sh, which
# stops BBM's instance and starts ours, as the user did by hand.
# Remove with: rm -r /var/addons/wacallsebm  (undo now: /var/rootdata/wacalls-ebm/restore.sh or reboot)
D=/var/rootdata/wacalls-ebm
LOG=$D/boot.log
export PATH=/accounts/1000/shared/misc/berrycore/bin:$PATH LD_LIBRARY_PATH=/accounts/1000/shared/misc/berrycore/bin:/accounts/1000/shared/misc/berrycore/lib
# Addons inherit a 3 MB data limit (see btquiet, volte): lift it for the engine.
ulimit -d unlimited 2>/dev/null
echo "=== $(date) addon start, data limit $(ulimit -d)" >> $LOG

running() { pidin -f "a n" ar | grep -v grep | grep -q "$1"; }

if running 'EBMService.wa'; then
    echo "$(date) ours already running, nothing to do" >> $LOG
    exit 0
fi
# Wait for BBM's EBMService (4 min after power-on on 2026-11-03), max 10 min,
# so that swap.sh stops it instead of racing it; then swap anyway.
n=0
while ! running 'app/native/ebmservice.conf' && [ $n -lt ${WAIT_STEPS:-60} ]; do
    sleep 10
    n=$((n + 1))
done
echo "$(date) BBM EBMService seen after ${n}0 s" >> $LOG
/bin/sh $D/swap.sh >> $LOG 2>&1 < /dev/null
# Check that ours stays up (registration is logged by the gateway).
sleep 60
if running 'EBMService.wa'; then
    echo "$(date) OK: EBMService.wa running" >> $LOG
else
    echo "$(date) FAIL: EBMService.wa not running, see $D/out.log" >> $LOG
fi
