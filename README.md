# Mako
![CI](https://github.com/makodb/mako/actions/workflows/ci.yml/badge.svg)

## Quickstart 

This is tested on Debian 12 and Ubuntu 22.04+.

Recursive clone everything 

```bash
git clone --recursive https://github.com/makodb/mako.git
```

Install all dependencies

```
bash apt_packages.sh
```

Configure and compile

```bash
make 
```
You should now see libmako.a and a few examples in the build folder, and run all examples via `./ci/ci.sh all`



Testing out MakoCon

```bash
./build/makoCon 
```
<!-- 
Run the helloworld:

```bash
./build/helloworld
```

Config hosts
```bash
# if Multi-servers: Update bash/ips_{p1|p2|leader|learner}, bash/ips_{p1|p2|leader|learner}.pub, n_partitions 
bash ./src/mako/update_config.sh 
```

## Experiment Runner

The `run_experiment.py` script automates the compilation and execution of distributed system experiments.

### Setup

The script uses `sshpass` by default for SSH authentication. Set your password as an environment variable:

```bash
export SSHPASS="your_password"
```

Make sure can you ssh your all servers with each other without password

### Compile and Run

```bash
# compile
# If the error <command-line>: fatal error: src/mako/masstree/config.h appears on the first run, rerun the script.
./run_experiment.py --shards 1 --threads 6 --ssh-user $USER --dry-run --only-compile
bash experiment_s1_norepl_t6_tpcc_r30s_compile-only.sh

# run
# all results are under ./results/*.log
./run_experiment.py --shards 1 --threads 6 --ssh-user $USER --dry-run --skip-compile
bash experiment_s1_norepl_t6_tpcc_r30s_no-compile.sh
sleep 1
tail -f *./results/*.log

# kill
./run_experiment.py --shards 1 --threads 6 --ssh-user $USER --cleanup-only

# more help
./run_experiment.py --help
```

### TODOs
 - TODO replace paxos waf script with standard cmake build


## Notes
1. we use nfs to sync some data, e.g., we use nfs to control all worker threads execute at the roughly same time (we used memcached in the past and removed this external dependencies)
2. for erpc, we add pure ethernet support so that you can use widely adopted sockets
```
cd ./third-party/erpc
make clean
cmake . -DTRANSPORT=fake -DROCE=off -DPERF=off
make -j10

cd ~/janus
echo "eth" > env.txt

sudo for bash/shard.sh is not rquired for socket-based transport
``` 

## Helloworld - Minimal Example

Here's a minimal example to get you started with Mako:

#### Step 1: Create `helloworld.cc`

```cpp
// helloworld.cc - A minimal Mako database example
#include <iostream>
#include <mako.hh>

using namespace std;

int main() {
    // 1. Create database instance
    abstract_db *db = new mbta_wrapper;
    
    // 2. Set up configuration (required for sharding)
    config = new transport::Configuration(
        "./src/mako/config/local-shards1-warehouses1.yml"
    );
    
    // 3. Initialize thread context
    scoped_db_thread_ctx ctx(db, false);
    mbta_ordered_index::mbta_type::thread_init();
    
    // 4. Open or create a table
    abstract_ordered_index *table = db->open_index("hello_table", 1, false, false);
    
    // 5. Set up persistent arena and transaction buffer
    str_arena arena;
    string txn_buf;
    txn_buf.reserve(str_arena::MinStrReserveLength);
    txn_buf.resize(db->sizeof_txn_object(0));
    
    // 6. Perform a write transaction
    {
        void *txn = db->new_txn(0, arena, (void*)txn_buf.data());
        scoped_str_arena s_arena(arena);  // RAII for arena management
        
        try {
            // Write a key-value pair
            string key = "hello";
            string actual_value = "world!";
            // Mako requires padding for internal versioning
            string padded_value = actual_value + string(mako::EXTRA_BITS_FOR_VALUE, '\0');
            table->put(txn, key, StringWrapper(padded_value));
            
            // Commit the transaction
            db->commit_txn(txn);
            cout << "Successfully wrote: " << key << " -> " << actual_value << endl;
            
        } catch (abstract_db::abstract_abort_exception &ex) {
            cout << "Transaction aborted!" << endl;
            db->abort_txn(txn);
        }
    }
    
    // 7. Perform a read transaction
    {
        void *txn = db->new_txn(0, arena, (void*)txn_buf.data());
        scoped_str_arena s_arena(arena);  // RAII for arena management
        
        try {
            // Read the value
            string key = "hello";
            string value = "";
            table->get(txn, key, value);
            
            // Commit the transaction
            db->commit_txn(txn);
            
            // Extract the actual value (removing null padding)
            size_t actual_length = value.find('\0');
            if (actual_length == string::npos) {
                actual_length = value.length();
            }
            string actual_value = value.substr(0, actual_length);
            cout << "Successfully read: " << key << " -> " << actual_value << endl;
            
        } catch (abstract_db::abstract_abort_exception &ex) {
            cout << "Transaction aborted!" << endl;
            db->abort_txn(txn);
        }
    }
    
    // 8. Clean up
    delete config;
    delete db;
    
    cout << "Hello World example completed!" << endl;
    return 0;
}
```

#### Step 2: Compile as Standalone Program

You can compile the helloworld example as a standalone program without modifying CMakeLists.txt:

```bash
# From project root, after building libmako.a with 'make'
g++ -std=c++17 \
    -I./src/mako \
    -I./src/mako/masstree \
    -I./src \
    -I./third-party/erpc/src \
    -I./third-party/erpc/third_party/asio/include \
    -I. \
    -DCONFIG_H=\"./src/mako/config/config-perf.h\" \
    -DERPC_FAKE=true \
    -include ./src/mako/masstree/config.h \
    -o helloworld \
    examples/helloworld.cc \
    ./build/libmako.a \
    ./build/third-party/erpc/liberpc.a \
    ./rust-lib/target/release/librust_redis.a \
    ./build/libtxlog.so \
    -lyaml-cpp -lpthread -lnuma -levent_pthreads -levent \
    -lboost_fiber -lboost_context -lboost_system -lboost_thread \
    -ljemalloc -lrt -ldl

# Run the example (from project root)
./helloworld
```

Alternatively, if you prefer using CMake (modifying CMakeLists.txt):

#### Step 3: Build with CMake

Add this line to `CMakeLists.txt` (around line 573):
```cmake
add_apps(helloworld examples/helloworld.cc)
```

Then build and run:
```bash
# From project root
cd build
cmake ..
make helloworld

# Run the example (from project root)
./build/helloworld
```

### Key Concepts

#### Transaction Management
- **Begin Transaction**: `db->new_txn(...)` creates a new transaction
- **Commit**: `db->commit_txn(txn)` commits changes
- **Abort**: `db->abort_txn(txn)` rolls back changes
- **Exception Handling**: Always wrap transactions in try-catch blocks

#### Table Operations
- **Open Table**: `db->open_index(name, ...)` opens or creates a table
- **Put**: `table->put(txn, key, value)` writes a key-value pair
- **Get**: `table->get(txn, key, value)` reads a value
- **Scan**: `table->scan(...)` iterates over a range of keys

#### Memory Management
- **str_arena**: Thread-local memory arena for efficient allocation
- **Transaction Buffer**: Pre-allocated buffer for transaction metadata
- **scoped_db_thread_ctx**: RAII wrapper for thread initialization
- **scoped_str_arena**: RAII wrapper for arena management

-->