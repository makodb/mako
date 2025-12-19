
rm -f a1.log a2.log a3.log a4.log

# Kill any lingering processes
killall simplePaxos 2>/dev/null
sleep 1

# Start processes with proper synchronization
echo "Starting p1 (follower)..."
./build/simplePaxos p1 > a2.log 2>&1 &
p1_pid=$!
sleep 5  # Give p1 time to fully initialize

echo "Starting p2 (follower)..."
./build/simplePaxos p2 > a3.log 2>&1 &
p2_pid=$!
sleep 5  # Give p2 time to fully initialize

echo "Starting learner..."
./build/simplePaxos learner > a4.log 2>&1 &
learner_pid=$!
sleep 5  # Give learner time to fully initialize

# Start leader last after all followers are ready
echo "Starting localhost (leader)..."
./build/simplePaxos localhost > a1.log 2>&1 &
localhost_pid=$!

echo "Waiting for completion..."
# Wait longer for all processes to complete - needs more time for 300 messages across 3 partitions
sleep 40 

tail -n 1 a1.log a2.log a3.log a4.log

# Check if all log files contain PASS keyword (with or without ANSI color codes)
echo ""
echo "Checking test results..."
failed=0
for log in a1.log a2.log a3.log a4.log; do
    if [ -f "$log" ]; then
        # Check for PASS with or without ANSI color codes
        if grep -E "PASS|\\[32mPASS\\[0m" "$log" > /dev/null; then
            echo "$log: PASS found ✓"
        else
            echo "$log: PASS not found ✗"
            failed=1
        fi
    else
        echo "$log: File not found ✗"
        failed=1
    fi
done

if [ $failed -eq 1 ]; then
    echo "Check failed - not all files contain PASS"
    exit 1
else
    echo "All checks passed!"
fi

