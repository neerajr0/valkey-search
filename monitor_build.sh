#!/bin/bash
# Monitor build CPU/memory usage and kill if excessive
CPU_THRESHOLD=95
MEM_THRESHOLD=90
SUSTAINED_COUNT=0
SUSTAINED_LIMIT=3
INTERVAL=10

echo "=== Build Resource Monitor Started ==="
echo "CPU threshold: ${CPU_THRESHOLD}% (sustained ${SUSTAINED_LIMIT} checks)"
echo "Memory threshold: ${MEM_THRESHOLD}%"
echo "Check interval: ${INTERVAL}s"
echo "======================================="

while true; do
    CPU=$(top -bn1 | grep "Cpu(s)" | awk '{print $2}' | cut -d. -f1)
    MEM=$(free | grep Mem | awk '{printf "%.0f", $3/$2 * 100}')
    TIMESTAMP=$(date '+%H:%M:%S')

    BUILD_PID=$(pgrep -f "ninja|make|cmake --build" | head -1)
    if [ -z "$BUILD_PID" ]; then
        echo "[$TIMESTAMP] No build process detected. Monitor exiting."
        break
    fi

    echo "[$TIMESTAMP] CPU: ${CPU}% | MEM: ${MEM}% | Build PID: ${BUILD_PID}"

    if [ "$MEM" -gt "$MEM_THRESHOLD" ]; then
        echo "[$TIMESTAMP] *** MEMORY THRESHOLD EXCEEDED (${MEM}% > ${MEM_THRESHOLD}%) ***"
        pkill -TERM -f "ninja|make|cmake --build" 2>/dev/null
        sleep 2
        pkill -KILL -f "ninja|make|cmake --build" 2>/dev/null
        echo "[$TIMESTAMP] Build killed due to excessive memory usage."
        break
    fi

    if [ "$CPU" -gt "$CPU_THRESHOLD" ]; then
        SUSTAINED_COUNT=$((SUSTAINED_COUNT + 1))
        echo "[$TIMESTAMP] High CPU (${SUSTAINED_COUNT}/${SUSTAINED_LIMIT})"
        if [ "$SUSTAINED_COUNT" -ge "$SUSTAINED_LIMIT" ]; then
            echo "[$TIMESTAMP] *** SUSTAINED CPU THRESHOLD EXCEEDED ***"
            pkill -TERM -f "ninja|make|cmake --build" 2>/dev/null
            sleep 2
            pkill -KILL -f "ninja|make|cmake --build" 2>/dev/null
            echo "[$TIMESTAMP] Build killed due to sustained high CPU."
            break
        fi
    else
        SUSTAINED_COUNT=0
    fi

    sleep $INTERVAL
done
echo "=== Monitor finished ==="
