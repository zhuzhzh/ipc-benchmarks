# IPC Performance Benchmarks

This directory contains benchmarks for various Inter-Process Communication (IPC) methods on a single host. The focus is on measuring latency and throughput for different IPC implementations.

## Benchmark Programs

### 1. Shared Memory (`shm_test.cc`)
- Direct shared memory implementation using POSIX shared memory
- SPSC (Single Producer Single Consumer) queue
- Minimal overhead, optimized for latency
- Cache-line aligned data structures to avoid false sharing

### 2. ZeroMQ (`zmq_test.cc`)
- Using ZeroMQ (cppzmq) for IPC
- REQ-REP pattern
- Supports both TCP and IPC (Unix Domain Socket) transports

### 3. NNG (`nng_test.cc`)
- Using nanomsg-next-generation
- PAIR pattern
- Supports both TCP and IPC transports

### 4. cpp-ipc (`cppipc_test.cc`)
#### Channel Mode (cppipc_channel_test)
- Single direction communication
- Producer-consumer pattern
- Shared memory based
- Lock-free implementation

#### Route Mode (cppipc_route_test)
- Optimized for SPSC (Single Producer Single Consumer)
- Supports one writer and up to 32 readers
- Lock-free shared memory implementation
- Better for unidirectional communication

### 5. AlephZero (`ale_test.cc`)
- Using AlephZero library
- RPC pattern
- Based on shared memory with robust error handling

### 6. Boost.Interprocess
#### Message Queue (boost_mq_test)
- Uses POSIX message queue
- Fixed message size
- Blocking operations
- High performance for small messages

#### Shared Memory (boost_shm_test)
- Direct shared memory access
- Custom synchronization
- Memory mapped file based
- Good for large messages

## Test Methodology

Each benchmark program:
1. Creates a server and client process
2. Performs warmup iterations (100 rounds)
3. Measures round-trip latency for 1000 messages
4. Calculates statistics:
   - Average latency
   - P50/P90/P99 latencies
   - Min/Max latencies
   - Messages per second throughput

Message size is fixed at 32 bytes for consistent comparison.

## Performance Results

### Latency (microseconds)

| Implementation | Average | P50    | P90    | P99     | Min    | Max     | Throughput (msg/s) |
|---------------|---------|--------|--------|---------|---------|---------|-------------------|
| Shared Memory | 0.70    | 0.55   | 0.80   | 1.50    | 0.50    | 35.66   | 2,875,100        |
| cpp-ipc chan  | 7.10    | 5.00   | 6.34   | 53.22   | 3.71    | 241.69  | 281,793          |
| cpp-ipc route | 7.26    | 4.37   | 6.28   | 102.99  | 3.66    | 250.77  | 275,616          |
| Boost MQ      | 4.34    | 0.97   | 7.94   | 55.57   | 0.64    | 211.73  | 461,245          |
| Boost SHM     | 69.08   | 63.18  | 91.00  | 177.47  | 27.35   | 353.38  | 28,953           |
| ZeroMQ (IPC)  | 261.8   | 250.3  | 333.4  | 428.5   | 173.4   | 582.4   | 7,638            |
| NNG (IPC)     | 427.5   | 417.7  | 503.8  | 639.9   | 268.5   | 807.9   | 4,678            |
| AlephZero     | 526.9   | 498.3  | 629.6  | 869.3   | 335.8   | 1825.9  | 3,796            |

### Key Findings

1. Raw shared memory implementation achieves exceptional latency (~0.7μs)
   - Sub-microsecond median latency (P50: 0.55μs)
   - Very consistent performance (90% of requests under 0.80μs)
   - Occasional spikes up to 35.66μs, likely due to system interrupts
   - Impressive throughput of 2.87M messages/second
   - Key optimizations:
     * Lock-free SPSC queue with minimal overhead
     * Fixed-size messages (no memory allocation)
     * Spin-waiting with CPU pause instruction
     * Cache-line alignment to prevent false sharing
   - Limitations:
     * No error handling or recovery mechanisms
     * Fixed message size only
     * Single producer/consumer only
     * High CPU usage due to spin-waiting

2. cpp-ipc implementations show excellent performance:
   - Channel (`ipc::channel`):
     * Very consistent performance (~7.1μs average)
     * Good P99 latency (53.22μs)
     * High throughput (281K msg/s)
     * Suitable for multi-producer multi-consumer
   - Route (`ipc::route`):
     * Similar average latency (~7.3μs)
     * Slightly higher P99 (102.99μs)
     * Good throughput (275K msg/s)
     * Supports one-to-many communication

3. Message brokers show significant overhead:
   - ZeroMQ: ~262μs average latency
   - NNG: ~428μs average latency
   - Higher latency due to protocol overhead and socket operations
   - More feature-rich but at the cost of performance

4. AlephZero shows highest latency (~527μs):
   - Prioritizes robustness and error handling
   - Complex protocol for ensuring message delivery
   - Higher variance in latency (335μs to 1826μs)

5. Boost.Interprocess shows interesting performance characteristics:
   - Message Queue (`boost::interprocess::message_queue`):
     * Surprisingly good performance (~4.3μs average)
     * Very low minimum latency (0.64μs)
     * Good throughput (461K msg/s)
     * Built-in message framing and synchronization
   - Shared Memory (`boost::interprocess::shared_memory_object`):
     * Higher latency (~69μs average)
     * More consistent performance (less variance)
     * Additional overhead from mutex and condition variables
     * More complex implementation but safer

## Building and Running

```bash
# Build all benchmarks
g++ -o shm_test ./shm_test.cc -pthread -l
g++ -o boost_shm_test boost_shm_test.cc -I${BOOST_HOME}/include -pthread -lrt 
g++ -o boost_mq_test boost_mq_test.cc -I${BOOST_HOME}/include -pthread -lrt 
g++ -o ./cppipc_channel_test ./cppipc_channel_test.cc -I${CPPIPC_HOME}/include -L${CPPIPC_HOME}/lib -lipc -pthread -lrt
g++ -o ./cppipc_route_test ./cppipc_route_test.cc -I/home/public/cppipc/include -L/home/public/cppipc/lib -lipc -lpthread -lrt
g++ ./zmq_test.cc -o zmq_test -I${ZEROMQ_HOME}/include -L${ZEROMQ_HOME}/lib64 -lzmq -pthread -lrt
g++ ./nng_test.cc -o nng_test -I${NNG_HOME}/include -L${NNG_HOME}/lib64 -lnng -pthread -lrt 
g++ -o ale_test ale_test.cc -I${ALEPHZERO_HOME}/include -L${ALEPHZERO_HOME}/lib -lalephzero -pthread 

# Run individual tests
./shm_test
./cppipc_test
./zmq_test
./nng_test
./ale_test
```

## Dependencies

- C++17 compiler
- ZeroMQ (cppzmq)
- nanomsg-next-generation
- cpp-ipc
- AlephZero

## Notes

1. All tests run on the same host to focus on IPC performance
2. Results may vary based on hardware and system load
3. Higher latency implementations often provide:
   - Better error handling and recovery
   - More sophisticated messaging patterns
   - Better scalability for complex systems
4. Choose implementation based on your requirements:
   - Raw shared memory: When absolute minimum latency is critical (<1μs)
   - cpp-ipc: When balance of performance and safety is needed (~7μs)
   - ZMQ/NNG: When advanced messaging patterns are required (~200-400μs)
   - AlephZero: When robustness is more important than latency (~500μs)

## Performance Trade-offs

### Communication Mechanism
- **Raw SHM**: Minimal SPSC queue directly on shared memory, almost no overhead
- **cpp-ipc**: More complex channel abstraction with complete feature set
- **Impact**: ~6μs additional latency for better abstraction and safety

### Memory Management
- **Raw SHM**: Fixed-size messages, no allocation, direct queue storage
- **cpp-ipc**: Variable-size messages, requires allocation and copying
- **Impact**: Memory operations contribute to higher latency and variance

### Synchronization
- **Raw SHM**: Spin-waiting with CPU pause, lowest latency but high CPU usage
- **cpp-ipc**: Balanced synchronization (condition variables/semaphores)
- **Impact**: Trade-off between CPU usage and latency

### Production Readiness
- **Raw SHM**: Benchmark-optimized, minimal implementation
- **cpp-ipc**: Production-ready with full feature set
- **Considerations**:
  * Buffer overflow handling
  * Process crash recovery
  * Variable message size support
  * Multi-producer/consumer scenarios
  * Error handling and reporting
  * Resource cleanup

### Optimization Possibilities
- **cpp-ipc** performance could be improved for specific use cases:
  * Using fixed message sizes
  * Reducing error checking overhead
  * Tuning buffer sizes and strategies
  * Aggressive compiler optimizations
- However, some overhead is inherent to providing a robust, general-purpose solution

### Boost.Interprocess Trade-offs

#### Message Queue vs Shared Memory
- **Message Queue**:
  * Simpler API and usage
  * Better performance in this test
  * Built-in message framing
  * Limited by system message queue settings
  * Good for small messages

- **Shared Memory**:
  * More flexible memory management
  * Direct memory access possible
  * Requires manual synchronization
  * Better for large data transfers
  * More control over implementation details

#### Implementation Differences
- **Message Queue**:
  * Uses system V message queues or POSIX mq
  * Kernel-supported message passing
  * Built-in synchronization
  * Fixed message size limits

- **Shared Memory**:
  * Manual memory management
  * Custom synchronization required
  * More overhead from mutex/condition variables
  * No inherent size limitations

### cpp-ipc Trade-offs

#### Channel vs Route
- **Channel**:
  * Multi-producer multi-consumer support
  * Better P99 latency
  * Slightly higher throughput
  * More consistent performance
  * Good for load balancing scenarios

- **Route**:
  * Single-producer multi-consumer (up to 32 receivers)
  * Simple broadcast pattern
  * Slightly higher tail latency
  * Good for pub/sub scenarios
  * Simpler synchronization (single writer)

#### Implementation Characteristics
- **Common Features**:
  * Lock-free queue design
  * Shared memory based
  * Zero-copy for local communication
  * Efficient memory management

- **Performance Considerations**:
  * Both achieve sub-10μs average latency
  * Channel better for high-load scenarios
  * Route better for broadcast patterns
  * Both suitable for low-latency requirements

---

## References

- [cpp-ipc](https://github.com/mutouyun/cpp-ipc)
- [AlephZero](https://github.com/alephzero/alephzero)
- [ZeroMQ](https://github.com/zeromq/libzmq)
- [nanomsg-next-generation](https://github.com/nanomsg/nng)
- [Boost.Interprocess message_queue](https://www.boost.org/doc/libs/1_87_0/doc/html/interprocess/synchronization_mechanisms.html#interprocess.synchronization_mechanisms.message_queue)
- [Boost.Interprocess shared_memory](https://www.boost.org/doc/libs/1_87_0/doc/html/interprocess/sharedmemorybetweenprocesses.html#interprocess.sharedmemorybetweenprocesses.sharedmemory)