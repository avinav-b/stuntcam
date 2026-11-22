#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# metro.sh start|stop — starts or stops Metro for fixtures/expo on :$EXPO_PORT (default 8099).
#   It only uses that port and only kills the pids it recorded. The pid files and logs go to build/.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p $ROOT/build
cd $ROOT/build
EXPO_PORT=${EXPO_PORT:-8099}

if [[ $1 == stop ]]; then
  for f in metro-*.pid; do
    # a server that is gone: nothing to stop
    [[ -f $f ]] && kill $(cat $f) 2>/dev/null
    rm -f $f
  done
  exit 0
fi

# Each entry is <fixture dir>:<port>.
for pair in expo:$EXPO_PORT; do
  fixture=${pair%%:*}
  port=${pair##*:}
  if lsof -tiTCP:$port -sTCP:LISTEN >/dev/null; then
    echo "port $port busy, not touching"
    continue
  fi
  (cd $ROOT/fixtures/$fixture && CI=1 nohup npx expo start --port $port --clear > $ROOT/build/metro-$fixture.log 2>&1 &)
done

for pair in expo:$EXPO_PORT; do
  fixture=${pair%%:*}
  port=${pair##*:}
  for i in {1..90}; do   # up to 90 s for Metro to answer
    curl -s localhost:$port/status | grep -q running && break
    sleep 1
  done
  lsof -tiTCP:$port -sTCP:LISTEN | head -1 > metro-$fixture.pid
  curl -s "http://localhost:$port/index.bundle?platform=ios&dev=true" -o /dev/null \
    -w "$fixture bundle %{http_code} %{size_download}\n"
done
