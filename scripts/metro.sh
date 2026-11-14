#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# metro.sh start|stop — Metro for fixtures/expo on :$EXPO_PORT (default 8099)
#   (only these ports, only our pids)
# pid files and logs go to build/
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; mkdir -p $ROOT/build; cd $ROOT/build
EXPO_PORT=${EXPO_PORT:-8099}
if [[ $1 == stop ]]; then for f in metro-*.pid; do [[ -f $f ]] && kill $(cat $f) 2>/dev/null; rm -f $f; done; exit 0; fi
for pair in expo:$EXPO_PORT; do d=${pair%%:*}; p=${pair##*:}
  if lsof -tiTCP:$p -sTCP:LISTEN >/dev/null; then echo "port $p busy, not touching"; continue; fi
  (cd $ROOT/fixtures/$d && CI=1 nohup npx expo start --port $p --clear > $ROOT/build/metro-$d.log 2>&1 &)
done
for pair in expo:$EXPO_PORT; do d=${pair%%:*}; p=${pair##*:}
  for i in {1..90}; do curl -s localhost:$p/status | grep -q running && break; sleep 1; done
  lsof -tiTCP:$p -sTCP:LISTEN | head -1 > metro-$d.pid
  curl -s "http://localhost:$p/index.bundle?platform=ios&dev=true" -o /dev/null -w "$d bundle %{http_code} %{size_download}\n"
done
