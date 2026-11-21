#!/bin/sh
# Stop our EBMService copy. BBM's own instance comes back on demand or at reboot.
export PATH=/accounts/1000/shared/misc/berrycore/bin:$PATH LD_LIBRARY_PATH=/accounts/1000/shared/misc/berrycore/bin:/accounts/1000/shared/misc/berrycore/lib
for p in $(pidin -f "a n" ar | grep 'EBMService.wa' | grep -v grep | awk '{print $1}'); do echo "stopping ours $p"; kill $p; done
