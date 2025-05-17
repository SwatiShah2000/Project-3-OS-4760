# Ensure IPC resources are cleared before starting
echo "Cleaning up any existing IPC resources..."
ipcs -m | grep $(whoami) | awk '{print $2}' | xargs -r ipcrm -m
ipcs -q | grep $(whoami) | awk '{print $2}' | xargs -r ipcrm -q
echo "Clean-up complete."

# Make sure the project is compiled
echo "Compiling project..."
make clean
make
echo "Compilation complete."

# Run several test cases
echo "===== Running Test Case 1: Basic Functionality ====="
echo "Running with default parameters..."
./oss -f test1.log
echo "Test 1 completed. Check test1.log for results."

echo "===== Running Test Case 2: Multiple Simultaneous Processes ====="
echo "Running with 10 total processes, 5 simultaneous..."
./oss -n 10 -s 5 -f test2.log
echo "Test 2 completed. Check test2.log for results."

echo "===== Running Test Case 3: Short Launch Interval ====="
echo "Running with 8 processes, 3 simultaneous, 200ms launch interval..."
./oss -n 8 -s 3 -i 200 -f test3.log
echo "Test 3 completed. Check test3.log for results."

echo "===== Running Test Case 4: Long Process Lifetimes ====="
echo "Running with 5 processes, 2 simultaneous, 10 second max lifetime..."
./oss -n 5 -s 2 -t 10 -f test4.log
echo "Test 4 completed. Check test4.log for results."

# Check for remaining IPC resources
echo "Checking for any remaining IPC resources..."
ipcs -m | grep $(whoami)
ipcs -q | grep $(whoami)

# Clean up remaining resources if any
echo "Final cleanup..."
ipcs -m | grep $(whoami) | awk '{print $2}' | xargs -r ipcrm -m
ipcs -q | grep $(whoami) | awk '{print $2}' | xargs -r ipcrm -q

echo "All tests completed."
