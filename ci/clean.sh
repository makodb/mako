ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i simplePaxos | awk "{print \$2}" | xargs kill -9 2>/dev/null

make clean
rm -rf ./out-perf.masstree/*
rm -rf ./src/mako/out-perf.masstree/*
rm -rf build/*
