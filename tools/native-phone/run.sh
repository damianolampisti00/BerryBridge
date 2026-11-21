#!/bin/sh
# Run our own copy of EBMService (BBM Video engine) against the Berry Bridge SIP relay.
# Usage: run.sh   (as root; stop the BBM instance first via swap.sh)
D=/var/rootdata/wacalls-ebm
SB=${BBM_SANDBOX:-$(ls -d /accounts/1000/appdata/sys.bbm.* 2>/dev/null | head -n 1)}
if [ -z "$SB" ] || [ ! -d "$SB" ]; then
    echo "run.sh: BBM appdata sandbox not found under /accounts/1000/appdata/sys.bbm.*" >&2
    exit 1
fi
# Our fake BBID user (the patched binary reads /pps/services/wbid/status)
mkdir -p /pps/services/wbid
printf '@status\nusername::wacalls\nemail::wacalls@wacalls.local\nfirstname::WhatsApp\nlastname::Calls\nscreenname::WhatsApp\n' > /pps/services/wbid/status
export HOME=$SB/data TMPDIR=$SB/tmp SANDBOX=$SB PERIMETER=personal PERIMETER_HOME=/accounts/1000 PPSDIR=/pps
export _SERVICEPATH=/pps/services/rtc/videochat/call/control
export LD_LIBRARY_PATH=$D/lib:/base/usr/lib:/base/lib:/proc/boot:/base/lib/dll:/base/usr/lib/qt4/lib:/radio/lib:/radio/lib/dll:/radio/usr/lib:/usr/lib/appserv
# libnobind.so: the engine binds its UDP sockets to one interface (bcm0) and
# address, which cuts them off the Berry Bridge relay on 127.0.0.1:40000.
# dialhelper: the Berry Bridge app's calls in the Phone app (127.0.0.1:5071);
# a second copy exits by itself, so engine restarts keep the first one.
$D/dialhelper >> $D/dialhelper.log 2>&1 < /dev/null &
export LD_PRELOAD=$D/lib/libnobind.so
cd "$SB"
exec $D/EBMService.wa $D/ebm.conf
