#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <a0/rpc.hpp>
#include <atomic>
#include <future>

const char* TOPIC_NAME = "benchmark";
const int MSG_SIZE = 32;
const int WARMUP_COUNT = 100;
const int TEST_COUNT = 1000;

void server_node() {
    // Create RPC server
    a0::RpcServer server(
        TOPIC_NAME,
        /* onrequest = */ [](a0::RpcRequest req) {
            // Echo back the message
            req.reply(req.pkt().payload());
        },
        /* oncancel = */ nullptr);

    // Keep server alive
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void client_node() {
    // Create RPC client
    a0::RpcClient client(TOPIC_NAME);
    
    // Add a small delay before starting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Pre-allocate send buffer
    std::string send_data(MSG_SIZE, 'A');
    
    // Warmup using blocking calls for simplicity
    std::cout << "Starting warmup...\n";
    for (int i = 0; i < WARMUP_COUNT; i++) {
        client.send_blocking(send_data);
    }
    std::cout << "Warmup completed\n";

    // Latency test
    std::vector<double> latencies;
    latencies.reserve(TEST_COUNT);

    std::cout << "Starting latency test...\n";
    for (int i = 0; i < TEST_COUNT; i++) {
        // 使用send_blocking来确保准确的延迟测量
        auto start = std::chrono::high_resolution_clock::now();
        client.send_blocking(send_data);
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
    
    double throughput = (1000000.0 / avg_latency) * 2;  // messages per second (multiply by 2 for roundtrip)

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