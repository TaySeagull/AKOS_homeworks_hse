#!/usr/bin/env bash
#
# Removes the IPC objects of the simulator that were left by a run killed
# with SIGKILL (a normal end or Ctrl+C removes them automatically):
#   /dev/shm/ed35_*        - POSIX shared memory objects
#   /dev/shm/sem.ed35*     - named POSIX semaphores
#   message queues /ed35mq_* (seen in /dev/mqueue if it is mounted)

found=0
for f in /dev/shm/ed35* /dev/shm/sem.ed35*; do
    [ -e "$f" ] || continue
    echo "removing $f"
    rm -f "$f"
    found=1
done

# message queues are files of the mqueue file system;
# they can be removed with rm when it is mounted
if [ -d /dev/mqueue ]; then
    for f in /dev/mqueue/ed35*; do
        [ -e "$f" ] || continue
        echo "removing $f"
        rm -f "$f"
        found=1
    done
else
    echo "note: /dev/mqueue is not mounted; to see the queues: sudo mkdir -p /dev/mqueue && sudo mount -t mqueue none /dev/mqueue"
fi

[ "$found" -eq 0 ] && echo "no IPC objects of the simulator found"
exit 0
