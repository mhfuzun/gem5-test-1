#!/bin/sh
set -x

echo "[INIT] uname:"
uname -a

echo "[INIT] ls -l /test:"
ls -l /test

echo "[INIT] file coremark:"
file /test/coremark || echo "file failed"

echo "[INIT] running coremark..."
/test/coremark
echo "[INIT] coremark returned $?"

exec /sbin/init
