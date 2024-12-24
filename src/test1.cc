// test_zmq_sharedmemory.cc
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <stdexcept>
#include <zmq.hpp>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <numeric>
#include <cerrno>
#include <cmath>
#include <immintrin.h> // For _mm_pause
#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

class IPCBase {
public:
    virtual ~IPCBase() = default;
    virtual void init(bool is_server) = 0;
    virtual void send(const void* data, size_t size) = 0;
    virtual bool recv(void* data, size_t size) = 0;
};

class ZmqIPC : public IPCBase {
public:
    ZmqIPC() : context_(1), socket_(context_, zmq::socket_type::dealer) {}
    
    void init(bool is_server) override {
        if (is_server) {
            socket_.bind("tcp://localhost:5555");
        } else {
            socket_.connect("tcp://localhost:5555");
        }
    }
    
    void send(const void* data, size_t size) override {
        auto result = socket_.send(zmq::buffer(data, size), zmq::send_flags::none);
        if (!result) {
            throw std::runtime_error("ZMQ send failed");
        }
    }
    
    bool recv(void* data, size_t size) override {
        zmq::message_t msg;
        auto result = socket_.recv(msg);
        if (result) {
            std::memcpy(data, msg.data(), size);
            return true;
        }
        return false;
    }

private:
    zmq::context_t context_;
    zmq::socket_t socket_;
};

class SharedMemoryIPC : public IPCBase {
public:
    struct alignas(64) Header {
        std::atomic<uint32_t> server_write_index;
        std::atomic<uint32_t> client_read_index;
        std::atomic<uint32_t> client_write_index;
        std::atomic<uint32_t> server_read_index;
        std::atomic<bool> server_ready;
        std::atomic<bool> client_ready;
        char padding[24]; // Pad to 64 bytes
    };
    
    explicit SharedMemoryIPC(size_t size) : shm_size_(size) {
        if (size == 0) {
            throw std::invalid_argument("Size cannot be zero");
        }
        data_size_ = size;
        total_size_ = sizeof(Header) + size * 2;  // Double buffer
        
        // Round up to page size
        page_size_ = sysconf(_SC_PAGESIZE);
        total_size_ = (total_size_ + page_size_ - 1) & ~(page_size_ - 1);
    }
    
    ~SharedMemoryIPC() {
        if (shared_mem_ != MAP_FAILED) {
            munmap(shared_mem_, total_size_);
        }
        if (is_server_) {
            shm_unlink("/my_shm");
            sem_unlink("/shm_server_ready");
            sem_unlink("/shm_client_ready");
        }
    }
    
    void init(bool is_server) override {
        is_server_ = is_server;
        
        // Create/open shared memory
        int fd;
        if (is_server) {
            // Cleanup existing shared memory
            shm_unlink("/my_shm");
            
            // Create new shared memory
            fd = shm_open("/my_shm", O_CREAT | O_RDWR, 0666);
            if (fd == -1) {
                throw std::runtime_error(std::string("Failed to create shared memory: ") + strerror(errno));
            }
            
            if (ftruncate(fd, total_size_) == -1) {
                close(fd);
                throw std::runtime_error(std::string("Failed to set shared memory size: ") + strerror(errno));
            }
            
            // Create semaphores
            server_sem_ = sem_open("/shm_server_ready", O_CREAT | O_EXCL, 0666, 0);
            if (server_sem_ == SEM_FAILED) {
                close(fd);
                throw std::runtime_error(std::string("Failed to create server semaphore: ") + strerror(errno));
            }
            
            client_sem_ = sem_open("/shm_client_ready", O_CREAT | O_EXCL, 0666, 0);
            if (client_sem_ == SEM_FAILED) {
                close(fd);
                sem_unlink("/shm_server_ready");
                throw std::runtime_error(std::string("Failed to create client semaphore: ") + strerror(errno));
            }
        } else {
            // Client: open existing shared memory
            for (int retry = 0; retry < 50; retry++) {
                fd = shm_open("/my_shm", O_RDWR, 0666);
                if (fd != -1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (fd == -1) {
                throw std::runtime_error("Timeout waiting for shared memory");
            }
            
            // Open semaphores
            int retry_count = 0;
            while ((server_sem_ = sem_open("/shm_server_ready", 0)) == SEM_FAILED) {
                if (++retry_count > 50) {
                    close(fd);
                    throw std::runtime_error("Timeout waiting for server semaphore");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            retry_count = 0;
            while ((client_sem_ = sem_open("/shm_client_ready", 0)) == SEM_FAILED) {
                if (++retry_count > 50) {
                    close(fd);
                    throw std::runtime_error("Timeout waiting for client semaphore");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        
        // Map shared memory with huge pages if available
        int flags = MAP_SHARED;
#ifdef MAP_HUGETLB
        flags |= MAP_HUGETLB;
#endif

        shared_mem_ = mmap(NULL, total_size_, PROT_READ | PROT_WRITE, flags, fd, 0);
        if (shared_mem_ == MAP_FAILED) {
            // Retry without huge pages
            shared_mem_ = mmap(NULL, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (shared_mem_ == MAP_FAILED) {
                close(fd);
                if (is_server_) shm_unlink("/my_shm");
                throw std::runtime_error(std::string("Failed to map shared memory: ") + strerror(errno));
            }
        }
        close(fd);
        
        // Initialize header
        header_ = static_cast<Header*>(shared_mem_);
        if (is_server) {
            header_->server_write_index.store(0, std::memory_order_relaxed);
            header_->client_read_index.store(0, std::memory_order_relaxed);
            header_->client_write_index.store(0, std::memory_order_relaxed);
            header_->server_read_index.store(0, std::memory_order_relaxed);
            header_->server_ready.store(false, std::memory_order_relaxed);
            header_->client_ready.store(false, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
        }
        
        // Setup data buffers aligned to cache line
        data_server_ = static_cast<uint8_t*>(shared_mem_) + ((sizeof(Header) + 63) & ~63);
        data_client_ = data_server_ + ((data_size_ + 63) & ~63);
        
        // Synchronize startup
        if (is_server) {
            std::cout << "Server waiting for client...\n";
            header_->server_ready.store(true, std::memory_order_release);
            sem_post(server_sem_);
            
            // Wait for client
            while (!header_->client_ready.load(std::memory_order_acquire)) {
                _mm_pause();
            }
            sem_wait(client_sem_);
            std::cout << "Client connected\n";
        } else {
            std::cout << "Client waiting for server...\n";
            // Wait for server
            while (!header_->server_ready.load(std::memory_order_acquire)) {
                _mm_pause();
            }
            sem_wait(server_sem_);
            
            // Signal ready
            header_->client_ready.store(true, std::memory_order_release);
            sem_post(client_sem_);
            std::cout << "Connected to server\n";
        }
        
        // Advise the kernel about our access pattern
        madvise(shared_mem_, total_size_, MADV_SEQUENTIAL);
    }
    
    void send(const void* data, size_t size) override {
        if (size > data_size_) {
            throw std::runtime_error("Data size exceeds buffer size");
        }
        
        if (is_server_) {
            // Server: non-blocking write
            memcpy(data_server_, data, size);
            header_->server_write_index.fetch_add(1, std::memory_order_release);
        } else {
            // Client: write to own buffer
            memcpy(data_client_, data, size);
            header_->client_write_index.fetch_add(1, std::memory_order_release);
        }
    }
    
    bool recv(void* data, size_t size) override {
        if (size > data_size_) {
            throw std::runtime_error("Data size exceeds buffer size");
        }
        
        if (is_server_) {
            uint32_t client_write = header_->client_write_index.load(std::memory_order_acquire);
            uint32_t server_read = header_->server_read_index.load(std::memory_order_relaxed);
            
            if (client_write > server_read) {
                memcpy(data, data_client_, size);
                header_->server_read_index.fetch_add(1, std::memory_order_release);
                return true;
            }
        } else {
            uint32_t server_write = header_->server_write_index.load(std::memory_order_acquire);
            uint32_t client_read = header_->client_read_index.load(std::memory_order_relaxed);
            
            if (server_write > client_read) {
                memcpy(data, data_server_, size);
                header_->client_read_index.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }
    
private:
    void* shared_mem_ = MAP_FAILED;
    Header* header_ = nullptr;
    uint8_t* data_server_ = nullptr;
    uint8_t* data_client_ = nullptr;
    size_t shm_size_;
    size_t data_size_;
    size_t total_size_;
    size_t page_size_;
    bool is_server_;
    sem_t* server_sem_ = SEM_FAILED;
    sem_t* client_sem_ = SEM_FAILED;
};

class UnixDomainSocketIPC : public IPCBase {
public:
    UnixDomainSocketIPC() {
        sock_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd_ < 0) {
            throw std::runtime_error(std::string("Failed to create socket: ") + strerror(errno));
        }
    }
    
    ~UnixDomainSocketIPC() {
        if (client_fd_ >= 0 && client_fd_ != sock_fd_) {
            close(client_fd_);
        }
        if (sock_fd_ >= 0) {
            close(sock_fd_);
        }
        if (is_server_) {
            unlink(socket_path_.c_str());
        }
    }
    
    void init(bool is_server) override {
        is_server_ = is_server;
        
        // Use /dev/shm for better performance and WSL compatibility
        // /dev/shm is a RAM-backed filesystem, providing better performance than /tmp
        // It's also more reliably shared between WSL and Windows
        socket_path_ = "/dev/shm/bench.sock";
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (socket_path_.length() >= sizeof(addr.sun_path)) {
            throw std::runtime_error("Socket path too long");
        }
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path)-1);
        
        if (is_server) {
            // Cleanup any existing socket file
            // Don't check return value as it may not exist
            unlink(socket_path_.c_str());
            
            // Set socket options before bind for better reliability
            int yes = 1;
            if (setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
                throw std::runtime_error(std::string("Failed to set SO_REUSEADDR: ") + strerror(errno));
            }
            
            if (bind(sock_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                throw std::runtime_error(std::string("Failed to bind: ") + strerror(errno));
            }
            
            // Ensure socket file has right permissions for other processes to connect
            if (chmod(socket_path_.c_str(), 0666) < 0) {
                throw std::runtime_error(std::string("Failed to set socket permissions: ") + strerror(errno));
            }
            
            if (listen(sock_fd_, 1) < 0) {
                throw std::runtime_error(std::string("Failed to listen: ") + strerror(errno));
            }
            
            std::cout << "Server waiting for connection on " << socket_path_ << "...\n";
            
            // Set accept timeout
            struct timeval tv;
            tv.tv_sec = 5;  // 5 seconds timeout
            tv.tv_usec = 0;
            setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
            
            // Accept with retry
            int retry_count = 0;
            const int max_retries = 50;  // 5 seconds with 100ms sleep
            while (retry_count < max_retries) {
                client_fd_ = accept(sock_fd_, NULL, NULL);
                if (client_fd_ >= 0) break;
                
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    throw std::runtime_error(std::string("Failed to accept: ") + strerror(errno));
                }
                
                if (retry_count % 10 == 0) {  // Print every second
                    std::cout << "Waiting for client... " 
                             << (max_retries - retry_count) / 10 << " seconds left\n";
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                retry_count++;
            }
            
            if (client_fd_ < 0) {
                throw std::runtime_error("Accept timeout after 5 seconds");
            }
            
            std::cout << "Client connected\n";
        } else {
            std::cout << "Client trying to connect...\n";
            
            // Set connect timeout
            struct timeval tv;
            tv.tv_sec = 5;  // 5 seconds timeout
            tv.tv_usec = 0;
            setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
            
            // Connect with retry
            int retry_count = 0;
            const int max_retries = 50;  // 5 seconds with 100ms sleep
            while (retry_count < max_retries) {
                if (connect(sock_fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    break;
                }
                
                if (errno != ENOENT && errno != ECONNREFUSED) {
                    throw std::runtime_error(std::string("Failed to connect: ") + strerror(errno));
                }
                
                if (retry_count % 10 == 0) {  // Print every second
                    std::cout << "Waiting for server... " 
                             << (max_retries - retry_count) / 10 << " seconds left\n";
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                retry_count++;
            }
            
            if (retry_count >= max_retries) {
                throw std::runtime_error("Connect timeout after 5 seconds");
            }
            
            client_fd_ = sock_fd_;
            std::cout << "Connected to server\n";
        }
        
        // Set socket options
        int flags = fcntl(client_fd_, F_GETFL, 0);
        if (flags == -1) {
            throw std::runtime_error(std::string("Failed to get socket flags: ") + strerror(errno));
        }
        if (fcntl(client_fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error(std::string("Failed to set non-blocking mode: ") + strerror(errno));
        }
    }
    
    void send(const void* data, size_t size) override {
        // Send size first
        uint32_t size_to_send = size;
        if (!send_all(&size_to_send, sizeof(size_to_send))) {
            throw std::runtime_error("Failed to send size");
        }
        
        // Then send data
        if (!send_all(data, size)) {
            throw std::runtime_error("Failed to send data");
        }
    }
    
    bool recv(void* data, size_t size) override {
        // Receive size first
        uint32_t size_to_recv;
        if (!recv_all(&size_to_recv, sizeof(size_to_recv))) {
            return false;
        }
        
        if (size_to_recv != size) {
            throw std::runtime_error("Size mismatch: expected " + std::to_string(size) + 
                                   ", got " + std::to_string(size_to_recv));
        }
        
        // Then receive data
        return recv_all(data, size);
    }

private:
    bool send_all(const void* data, size_t size) {
        const char* buf = static_cast<const char*>(data);
        size_t sent = 0;
        auto start = std::chrono::steady_clock::now();
        
        while (sent < size) {
            ssize_t ret = write(client_fd_, buf + sent, size - sent);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > 5) {
                        std::cerr << "Send timeout\n";
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }
                std::cerr << "Send error: " << strerror(errno) << std::endl;
                return false;
            }
            if (ret == 0) {
                std::cerr << "Connection closed during send\n";
                return false;
            }
            sent += ret;
        }
        return true;
    }
    
    bool recv_all(void* data, size_t size) {
        char* buf = static_cast<char*>(data);
        size_t received = 0;
        auto start = std::chrono::steady_clock::now();
        
        while (received < size) {
            ssize_t ret = read(client_fd_, buf + received, size - received);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > 5) {
                        std::cerr << "Receive timeout\n";
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }
                std::cerr << "Receive error: " << strerror(errno) << std::endl;
                return false;
            }
            if (ret == 0) {
                std::cerr << "Connection closed by peer\n";
                return false;
            }
            received += ret;
        }
        return true;
    }
    
    int sock_fd_ = -1;
    int client_fd_ = -1;
    bool is_server_ = false;
    std::string socket_path_;
};

class NngIPC : public IPCBase {
public:
    NngIPC() {
        socket_.id = 0;  // Initialize socket id to invalid value
    }
    
    ~NngIPC() {
        if (socket_.id != 0) {
            nng_close(socket_);
        }
    }

    void init(bool is_server) override {
        int rv;
        if (is_server) {
            if ((rv = nng_pair_open(&socket_)) != 0) {
                throw std::runtime_error("nng_push0_open: " + std::string(nng_strerror(rv)));
            }
            if ((rv = nng_listen(socket_, "ipc:///dev/shm/bench.ipc", nullptr, 0)) != 0) {
                throw std::runtime_error("nng_listen: " + std::string(nng_strerror(rv)));
            }
        } else {
            if ((rv = nng_pair_open(&socket_)) != 0) {
                throw std::runtime_error("nng_pull0_open: " + std::string(nng_strerror(rv)));
            }
            if ((rv = nng_dial(socket_, "ipc:///dev/shm/bench.ipc", nullptr, 0)) != 0) {
                throw std::runtime_error("nng_dial: " + std::string(nng_strerror(rv)));
            }
        }
        
        // Set send buffer size
        //if ((rv = nng_socket_set_int(socket_, NNG_OPT_SENDBUF, 16384)) != 0) {
        //    throw std::runtime_error("nng_socket_set_size (sendbuf): " + std::string(nng_strerror(rv)));
        //}

        //// Set receive buffer size
        //if ((rv = nng_socket_set_int(socket_, NNG_OPT_RECVBUF, 16384)) != 0) {
        //    throw std::runtime_error("nng_socket_set_size (recvbuf): " + std::string(nng_strerror(rv)));
        //}

        // Set send timeout (1 second)
        if ((rv = nng_socket_set_ms(socket_, NNG_OPT_SENDTIMEO, 10000)) != 0) {
            throw std::runtime_error("nng_socket_set_ms (sendtimeo): " + std::string(nng_strerror(rv)));
        }
    }

    void send(const void* data, size_t size) override {
        int rv;
        if ((rv = nng_send(socket_, const_cast<void*>(data), size, 0)) != 0) {
            throw std::runtime_error("nng_send: " + std::string(nng_strerror(rv)));
        }
    }

    bool recv(void* data, size_t size) override {
        size_t sz = size;
        int rv = nng_recv(socket_, data, &sz, NNG_FLAG_NONBLOCK);
        if (rv == NNG_EAGAIN) {
            return false;
        }
        if (rv != 0) {
            throw std::runtime_error("nng_recv: " + std::string(nng_strerror(rv)));
        }
        return true;
    }

private:
    nng_socket socket_;
};

void run_benchmark(IPCBase& ipc, bool is_server, size_t data_size, int iterations) {
    if (data_size == 0 || iterations <= 0) {
        throw std::invalid_argument("Invalid data_size or iterations");
    }

    std::vector<uint8_t> send_buf(data_size);
    std::vector<uint8_t> recv_buf(data_size);
    std::vector<double> latencies;
    
    // Initialize send buffer with pattern
    for (size_t i = 0; i < data_size; i++) {
        send_buf[i] = static_cast<uint8_t>(i & 0xFF);
    }
    
    try {
        ipc.init(is_server);
        
        if (is_server) {
            std::cout << "Server starting benchmark...\n";
            int success_count = 0;
            
            // Server loop - server sends first
            for (int i = 0; i < iterations; i++) {
                try {
                    ipc.send(send_buf.data(), data_size);
                    //if (i % 500 == 0) {
                    //    std::cout << "Server sent data " << i << "\n";
                    //}
                } catch (const std::exception& e) {
                    std::cerr << "Server send failed at iteration " << i << ": " << e.what() << "\n";
                    break;
                }
                
                bool received = false;
                for (int retries = 0; retries < 5000 && !received; retries++) {
                    received = ipc.recv(recv_buf.data(), data_size);
                    if (!received) {
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                    }
                }
                
                if (!received) {
                    std::cerr << "Server recv failed at iteration " << i << "\n";
                    break;
                }
                
                //if (i % 500 == 0) {
                //    std::cout << "Server received data " << i << "\n";
                //}
                
                // Verify received data
                bool data_valid = true;
                for (size_t j = 0; j < data_size; j++) {
                    if (recv_buf[j] != static_cast<uint8_t>(j & 0xFF)) {
                        std::cerr << "Data verification failed at byte " << j << "\n";
                        data_valid = false;
                        break;
                    }
                }
                
                if (data_valid) {
                    success_count++;
                }
            }
            std::cout << "Server completed benchmark: " << success_count << "/" 
                      << iterations << " successful iterations\n";
        } else {
            std::cout << "Client starting benchmark...\n";
            latencies.reserve(iterations);
            int success_count = 0;
            
            // Client loop - client receives first
            for (int i = 0; i < iterations; i++) {
                auto start = std::chrono::high_resolution_clock::now();
                
                bool received = false;
                for (int retries = 0; retries < 5000 && !received; retries++) {
                    received = ipc.recv(recv_buf.data(), data_size);
                    if (!received) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                
                if (!received) {
                    std::cerr << "Client recv failed at iteration " << i << "\n";
                    break;
                }
                
                //if (i % 500 == 0) {
                //    std::cout << "Client received data " << i << "\n";
                //}
                
                // Verify received data
                bool data_valid = true;
                for (size_t j = 0; j < data_size; j++) {
                    if (recv_buf[j] != static_cast<uint8_t>(j & 0xFF)) {
                        std::cerr << "Data verification failed at byte " << j << "\n";
                        data_valid = false;
                        break;
                    }
                }
                
                try {
                    ipc.send(send_buf.data(), data_size);
                    //if (i % 500 == 0) {
                    //    std::cout << "Client sent data " << i << "\n";
                    //}
                } catch (const std::exception& e) {
                    std::cerr << "Client send failed at iteration " << i << ": " << e.what() << "\n";
                    break;
                }
                
                auto end = std::chrono::high_resolution_clock::now();
                if (data_valid) {
                    double latency = std::chrono::duration_cast<std::chrono::nanoseconds>
                        (end - start).count();
                    latencies.push_back(latency);
                    success_count++;
                }
            }
            
            // Output statistics
            if (!latencies.empty()) {
                std::sort(latencies.begin(), latencies.end());
                double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
                double mean = sum / latencies.size();
                double p50 = latencies[latencies.size() * 50 / 100];
                double p90 = latencies[latencies.size() * 90 / 100];
                double p99 = latencies[latencies.size() * 99 / 100];
                
                std::cout << std::fixed << std::setprecision(2)
                        << "\nBenchmark Results:\n"
                        << "  Data size: " << data_size << " bytes\n"
                        << "  Iterations: " << success_count << "/" << iterations << " successful\n"
                        << "\nLatency Statistics (ns):\n"
                        << "  Mean: " << mean << "\n"
                        << "  P50:  " << p50 << "\n"
                        << "  P90:  " << p90 << "\n"
                        << "  P99:  " << p99 << "\n"
                        << "  Min:  " << latencies.front() << "\n"
                        << "  Max:  " << latencies.back() << "\n"
                        << "\nThroughput:\n"
                        << "  " << (data_size * success_count * 1e9 / sum) << " MB/s\n"
                        << "  " << (success_count * 1e9 / sum) << " ops/s\n";
            }
            std::cout << "Client completed benchmark\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Benchmark error: " << e.what() << std::endl;
        throw;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <ipc_type> <server|client> <data_size> <iterations>\n"
                  << "ipc_type: shm, zmq, uds, or nng\n";
        return 1;
    }

    std::string ipc_type = argv[1];
    bool is_server = std::string(argv[2]) == "server";
    size_t data_size = std::stoul(argv[3]);
    int iterations = std::stoi(argv[4]);

    try {
        std::unique_ptr<IPCBase> ipc;
        
        if (ipc_type == "shm") {
            ipc = std::make_unique<SharedMemoryIPC>(data_size);
        } else if (ipc_type == "zmq") {
            ipc = std::make_unique<ZmqIPC>();
        } else if (ipc_type == "uds") {
            ipc = std::make_unique<UnixDomainSocketIPC>();
        } else if (ipc_type == "nng") {
            ipc = std::make_unique<NngIPC>();
        } else {
            throw std::runtime_error("Unknown IPC type: " + ipc_type);
        }

        run_benchmark(*ipc, is_server, data_size, iterations);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}