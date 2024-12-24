#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/reqrep0/rep.h>

const char* URL = "ipc:///dev/shm/nng_test";
const int MSG_SIZE = 32;
const int WARMUP_COUNT = 100;
const int TEST_COUNT = 1000;

void fatal(const char* func, int rv) {
    std::cerr << func << " failed: " << nng_strerror(rv) << std::endl;
    exit(1);
}

void configure_socket(nng_socket sock) {
    int rv;
    
    // Set buffer sizes (maximum allowed is 8192)
    int bufsize = 8192;
    if ((rv = nng_socket_set_int(sock, NNG_OPT_RECVBUF, bufsize)) != 0) {
        fatal("nng_socket_set_int(recvbuf)", rv);
    }
    if ((rv = nng_socket_set_int(sock, NNG_OPT_SENDBUF, bufsize)) != 0) {
        fatal("nng_socket_set_int(sendbuf)", rv);
    }

    // Set timeouts (1 second)
    nng_duration timeout = 1000;
    if ((rv = nng_socket_set_ms(sock, NNG_OPT_SENDTIMEO, timeout)) != 0) {
        fatal("nng_socket_set_ms(sendtimeo)", rv);
    }
    if ((rv = nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, timeout)) != 0) {
        fatal("nng_socket_set_ms(recvtimeo)", rv);
    }
}

void server_node() {
    nng_socket sock;
    int rv;

    if ((rv = nng_rep0_open(&sock)) != 0) {
        fatal("nng_rep0_open", rv);
    }

    configure_socket(sock);

    if ((rv = nng_listen(sock, URL, nullptr, 0)) != 0) {
        fatal("nng_listen", rv);
    }

    // Pre-allocate receive buffer
    std::vector<char> buf(MSG_SIZE);
    
    // Echo back messages
    for (int i = 0; i < WARMUP_COUNT + TEST_COUNT; i++) {
        size_t sz = buf.size();
        if ((rv = nng_recv(sock, buf.data(), &sz, 0)) != 0) {
            fatal("nng_recv", rv);
        }
        if ((rv = nng_send(sock, buf.data(), sz, 0)) != 0) {
            fatal("nng_send", rv);
        }
    }
    
    nng_close(sock);
}

void client_node() {
    nng_socket sock;
    int rv;
    
    if ((rv = nng_req0_open(&sock)) != 0) {
        fatal("nng_req0_open", rv);
    }

    configure_socket(sock);

    // Add a small delay before connecting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if ((rv = nng_dial(sock, URL, nullptr, 0)) != 0) {
        fatal("nng_dial", rv);
    }

    // Add a small delay after connecting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Pre-allocate buffers
    std::vector<char> send_buf(MSG_SIZE, 'A');
    std::vector<char> recv_buf(MSG_SIZE);
    
    // Warmup
    std::cout << "Starting warmup...\n";
    for (int i = 0; i < WARMUP_COUNT; i++) {
        if ((rv = nng_send(sock, send_buf.data(), send_buf.size(), 0)) != 0) {
            fatal("nng_send", rv);
        }
        size_t sz = recv_buf.size();
        if ((rv = nng_recv(sock, recv_buf.data(), &sz, 0)) != 0) {
            fatal("nng_recv", rv);
        }
    }
    std::cout << "Warmup completed\n";

    // Latency test
    std::vector<double> latencies;
    latencies.reserve(TEST_COUNT);

    std::cout << "Starting latency test...\n";
    for (int i = 0; i < TEST_COUNT; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        
        if ((rv = nng_send(sock, send_buf.data(), send_buf.size(), 0)) != 0) {
            fatal("nng_send", rv);
        }
        
        size_t sz = recv_buf.size();
        if ((rv = nng_recv(sock, recv_buf.data(), &sz, 0)) != 0) {
            fatal("nng_recv", rv);
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
    
    double avg_latency = total / TEST_COUNT / 1000.0; // convert to microseconds
    double p50 = latencies[TEST_COUNT * 50 / 100] / 1000.0;
    double p90 = latencies[TEST_COUNT * 90 / 100] / 1000.0;
    double p99 = latencies[TEST_COUNT * 99 / 100] / 1000.0;
    double min_latency = latencies.front() / 1000.0;
    double max_latency = latencies.back() / 1000.0;
    
    double throughput = (1000000.0 / avg_latency) * 2; // messages per second (multiply by 2 for roundtrip)

    std::cout << "\nLatency Statistics (microseconds):\n"
              << "  Average: " << avg_latency << "\n"
              << "  P50:     " << p50 << "\n"
              << "  P90:     " << p90 << "\n"
              << "  P99:     " << p99 << "\n"
              << "  Min:     " << min_latency << "\n"
              << "  Max:     " << max_latency << "\n"
              << "\nThroughput: " << throughput << " messages/second\n";
    
    nng_close(sock);
}

int main() {
    std::thread server(server_node);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let server start
    std::thread client(client_node);

    server.join();
    client.join();
    
    return 0;
}
