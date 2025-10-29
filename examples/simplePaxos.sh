
rm a1.log a2.log a3.log a4.log
nohup ./build/simplePaxos localhost > a1.log 2>&1 &
sleep 1 
nohup ./build/simplePaxos learner > a4.log 2>&1 &
sleep 1
nohup ./build/simplePaxos p2 > a3.log 2>&1 &
sleep 1
nohup ./build/simplePaxos p1 > a2.log 2>&1 &

sleep 10 

tail -n 1 a1.log a2.log a3.log a4.log

# Check if all log files contain PASS keyword
echo ""
echo "Checking test results..."
failed=0
for log in a1.log a2.log a3.log a4.log; do
    if [ -f "$log" ]; then
        if grep -q "PASS" "$log"; then
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

