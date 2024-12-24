#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>

using namespace boost::interprocess;

const char* SHM_NAME = "benchmark_shm";
const int MSG_SIZE = 32;
const int WARMUP_COUNT = 100;
const int TEST_COUNT = 1000;

// 共享内存数据结构
struct SharedMemory {
    SharedMemory() 
        : ready(false), done(false), request_count(0), response_count(0) {}

    interprocess_mutex mutex;
    interprocess_condition request_ready;
    interprocess_condition response_ready;
    bool ready;
    bool done;
    uint32_t request_count;
    uint32_t response_count;
    char data[MSG_SIZE];
};

void server_node() {
    // 创建共享内存
    shared_memory_object shm(create_only, SHM_NAME, read_write);
    shm.truncate(sizeof(SharedMemory));
    mapped_region region(shm, read_write);
    
    // 构造共享内存对象
    SharedMemory* shared = new (region.get_address()) SharedMemory();
    
    // 处理请求
    for (int i = 0; i < WARMUP_COUNT + TEST_COUNT; i++) {
        scoped_lock<interprocess_mutex> lock(shared->mutex);
        
        // 等待请求
        while (!shared->ready && !shared->done) {
            shared->request_ready.wait(lock);
        }
        if (shared->done) break;
        
        // 处理请求（简单回显）
        shared->ready = false;
        shared->response_count = shared->request_count;
        
        // 通知响应就绪
        shared->response_ready.notify_one();
    }
    
    // 清理
    shared_memory_object::remove(SHM_NAME);
}

void client_node() {
    // 打开共享内存
    shared_memory_object shm(open_only, SHM_NAME, read_write);
    mapped_region region(shm, read_write);
    SharedMemory* shared = static_cast<SharedMemory*>(region.get_address());
    
    // 预分配缓冲区
    std::vector<char> send_buf(MSG_SIZE, 'A');
    
    // Warmup
    std::cout << "Starting warmup...\n";
    for (int i = 0; i < WARMUP_COUNT; i++) {
        scoped_lock<interprocess_mutex> lock(shared->mutex);
        
        // 发送请求
        std::memcpy(shared->data, send_buf.data(), MSG_SIZE);
        shared->request_count = i;
        shared->ready = true;
        shared->request_ready.notify_one();
        
        // 等待响应
        while (shared->ready) {
            shared->response_ready.wait(lock);
        }
    }
    std::cout << "Warmup completed\n";

    // Latency test
    std::vector<double> latencies;
    latencies.reserve(TEST_COUNT);

    std::cout << "Starting latency test...\n";
    for (int i = 0; i < TEST_COUNT; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        
        {
            scoped_lock<interprocess_mutex> lock(shared->mutex);
            
            // 发送请求
            std::memcpy(shared->data, send_buf.data(), MSG_SIZE);
            shared->request_count = WARMUP_COUNT + i;
            shared->ready = true;
            shared->request_ready.notify_one();
            
            // 等待响应
            while (shared->ready) {
                shared->response_ready.wait(lock);
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        latencies.push_back(duration.count());

        if ((i + 1) % 100 == 0) {
            std::cout << "Completed " << (i + 1) << " iterations\n";
        }
    }

    // 通知服务器结束
    {
        scoped_lock<interprocess_mutex> lock(shared->mutex);
        shared->done = true;
        shared->request_ready.notify_one();
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
}

int main() {
    std::thread server(server_node);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Let server start
    std::thread client(client_node);

    server.join();
    client.join();
    
    return 0;
} 