#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <immintrin.h>  // for _mm_pause
#include <thread>      // for std::this_thread::yield

// Cross-platform CPU pause
inline void cpu_pause() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

// 简单的SPSC队列实现
template<typename T, size_t Size>
class SPSCQueue {
    static_assert(Size && !(Size & (Size - 1)), "Size must be a power of 2");
    
    struct alignas(64) Item {  // 避免false sharing
        std::atomic<bool> valid{false};
        T data;
    };

    alignas(64) Item items[Size];
    alignas(64) std::atomic<size_t> write_idx{0};
    alignas(64) std::atomic<size_t> read_idx{0};

public:
    bool try_push(const T& data) {
        const size_t current = write_idx.load(std::memory_order_relaxed);
        Item& item = items[current & (Size - 1)];
        
        if (item.valid.load(std::memory_order_acquire)) {
            return false;  // 队列满
        }
        
        item.data = data;
        item.valid.store(true, std::memory_order_release);
        write_idx.store(current + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& data) {
        const size_t current = read_idx.load(std::memory_order_relaxed);
        Item& item = items[current & (Size - 1)];
        
        if (!item.valid.load(std::memory_order_acquire)) {
            return false;  // 队列空
        }
        
        data = item.data;
        item.valid.store(false, std::memory_order_release);
        read_idx.store(current + 1, std::memory_order_release);
        return true;
    }
};

// 共享内存通信结构
struct SharedMemory {
    alignas(64) SPSCQueue<uint64_t, 1024> request_queue;
    alignas(64) SPSCQueue<uint64_t, 1024> response_queue;
};

const char* SHM_NAME = "/shm_test";
const int MSG_SIZE = sizeof(uint64_t);
const int WARMUP_COUNT = 100;
const int TEST_COUNT = 1000;

SharedMemory* create_shared_memory() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        exit(1);
    }

    if (ftruncate(fd, sizeof(SharedMemory)) == -1) {
        perror("ftruncate");
        exit(1);
    }

    void* ptr = mmap(nullptr, sizeof(SharedMemory), 
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    return new (ptr) SharedMemory();
}

void server_node() {
    SharedMemory* shm = create_shared_memory();
    uint64_t msg;

    // Echo back messages
    for (int i = 0; i < WARMUP_COUNT + TEST_COUNT; i++) {
        while (!shm->request_queue.try_pop(msg)) {
            cpu_pause();  // 减少CPU使用
        }
        while (!shm->response_queue.try_push(msg)) {
            cpu_pause();
        }
    }
}

void client_node() {
    SharedMemory* shm = create_shared_memory();
    std::vector<double> latencies;
    latencies.reserve(TEST_COUNT);

    // Warmup
    std::cout << "Starting warmup...\n";
    for (int i = 0; i < WARMUP_COUNT; i++) {
        uint64_t msg = i;
        while (!shm->request_queue.try_push(msg)) {
            cpu_pause();
        }
        while (!shm->response_queue.try_pop(msg)) {
            cpu_pause();
        }
    }
    std::cout << "Warmup completed\n";

    // Latency test
    std::cout << "Starting latency test...\n";
    for (int i = 0; i < TEST_COUNT; i++) {
        uint64_t msg = i;
        auto start = std::chrono::high_resolution_clock::now();
        
        while (!shm->request_queue.try_push(msg)) {
            cpu_pause();
        }
        while (!shm->response_queue.try_pop(msg)) {
            cpu_pause();
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        latencies.push_back(duration.count());

        if ((i + 1) % 100 == 0) {
            std::cout << "Completed " << (i + 1) << " iterations\n";
        }
    }

    // Calculate statistics
    std::sort(latencies.begin(), latencies.end());
    double total = 0;
    for (double lat : latencies) {
        total += lat;
    }
    
    double avg_latency = total / TEST_COUNT / 1000.0;  // convert to microseconds
    double p50 = latencies[TEST_COUNT * 50 / 100] / 1000.0;
    double p90 = latencies[TEST_COUNT * 90 / 100] / 1000.0;
    double p99 = latencies[TEST_COUNT * 99 / 100] / 1000.0;
    double min_latency = latencies.front() / 1000.0;
    double max_latency = latencies.back() / 1000.0;
    
    double throughput = (1000000.0 / avg_latency) * 2;  // messages per second

    std::cout << "\nLatency Statistics (microseconds):\n"
              << "  Average: " << avg_latency << "\n"
              << "  P50:     " << p50 << "\n"
              << "  P90:     " << p90 << "\n"
              << "  P99:     " << p99 << "\n"
              << "  Min:     " << min_latency << "\n"
              << "  Max:     " << max_latency << "\n"
              << "\nThroughput: " << throughput << " messages/second\n";

    // Cleanup
    shm_unlink(SHM_NAME);
}

int main() {
    std::thread server(server_node);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread client(client_node);

    server.join();
    client.join();
    
    return 0;
}
