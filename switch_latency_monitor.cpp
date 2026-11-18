#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <mutex>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/errqueue.h>

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

struct PacketPayload {
    uint64_t magic;
    uint64_t seq_id;
};

constexpr size_t UDP_PAYLOAD_OFFSET = sizeof(ether_header) + sizeof(iphdr) + sizeof(udphdr);
static_assert(UDP_PAYLOAD_OFFSET + sizeof(PacketPayload) <= 64 - 4,
              "Measurement payload must fit in a 64-byte Ethernet frame without the FCS");

struct PendingPacket {
    uint64_t tx_ns = 0;
    uint16_t frame_size = 0;
    double elapsed_sec = 0.0;
};

struct FrameStats {
    uint64_t sent = 0;
    uint64_t received = 0;
    std::vector<double> latencies_us;
};

inline uint64_t timespec_to_ns(const struct timespec& ts) {
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

uint16_t ipv4_checksum(const void* data, size_t len) {
    const auto* ptr = reinterpret_cast<const uint16_t*>(data);
    uint32_t sum = 0;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0) {
        sum += *reinterpret_cast<const uint8_t*>(ptr);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

void get_iface_info(int sock, const char* iface_name, int& ifindex, uint8_t mac[6]) {
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        exit(EXIT_FAILURE);
    }
    ifindex = ifr.ifr_ifindex;

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        perror("SIOCGIFHWADDR");
        exit(EXIT_FAILURE);
    }
    std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
}

void enable_hw_timestamping(int sock, const char* iface_name) {
    struct ifreq ifr {};
    struct hwtstamp_config hwconfig {};

    std::strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);
    hwconfig.tx_type = HWTSTAMP_TX_ON;
    hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;

    ifr.ifr_data = reinterpret_cast<char*>(&hwconfig);
    if (ioctl(sock, SIOCSHWTSTAMP, &ifr) < 0) {
        hwconfig.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
        if (ioctl(sock, SIOCSHWTSTAMP, &ifr) < 0) {
            perror("SIOCSHWTSTAMP (verifique sudo/CAP_NET_ADMIN)");
        }
    }
}

uint64_t extract_hw_timestamp(struct msghdr* msg) {
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
            auto* ts = reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg));
            return timespec_to_ns(ts[2]);
        }
    }
    return 0;
}

void print_dashboard(const std::vector<size_t>& frame_sizes,
                     std::unordered_map<size_t, FrameStats>& stats_map,
                     std::mutex& mtx,
                     double elapsed_sec,
                     const std::string& csv_path,
                     bool clear_screen) {
    std::lock_guard<std::mutex> lock(mtx);

    if (clear_screen) {
        std::cout << "\033[2J\033[H";
    }

    std::cout << "=========================================================================================================\n";
    std::cout << " MONITOR DE LATENCIA DE SWITCH (10GbE HW TIMESTAMP) | Tempo: "
              << std::fixed << std::setprecision(1) << elapsed_sec << "s | Arquivo: " << csv_path << "\n";
    std::cout << " Pressione Ctrl+C para encerrar e salvar o relatorio final.\n";
    std::cout << "=========================================================================================================\n";
    std::cout << std::left
              << std::setw(10) << "Quadro"
              << std::setw(14) << "Env / Rec"
              << std::setw(10) << "Perda"
              << std::setw(13) << "Min (us)"
              << std::setw(13) << "Media (us)"
              << std::setw(13) << "P50 (us)"
              << std::setw(13) << "P99 (us)"
              << std::setw(13) << "Max (us)"
              << std::setw(12) << "Jitter(us)" << "\n";
    std::cout << "---------------------------------------------------------------------------------------------------------\n";

    for (size_t fsize : frame_sizes) {
        auto& st = stats_map[fsize];
        double loss_pct = (st.sent > 0) ? (100.0 * (st.sent - st.received) / st.sent) : 0.0;

        std::cout << std::left << std::setw(10) << (std::to_string(fsize) + "B")
                  << std::setw(14) << (std::to_string(st.sent) + "/" + std::to_string(st.received))
                  << std::setw(10) << (std::to_string(static_cast<int>(loss_pct)) + "%");

        if (!st.latencies_us.empty()) {
            std::vector<double> sorted = st.latencies_us;
            std::sort(sorted.begin(), sorted.end());

            double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
            double mean = sum / sorted.size();

            double sq_sum = 0.0;
            for (double v : sorted) {
                sq_sum += (v - mean) * (v - mean);
            }
            double jitter = std::sqrt(sq_sum / sorted.size());

            double min_v = sorted.front();
            double max_v = sorted.back();
            double p50 = sorted[static_cast<size_t>(sorted.size() * 0.50)];
            double p99 = sorted[static_cast<size_t>(sorted.size() * 0.99)];

            std::cout << std::fixed << std::setprecision(3)
                      << std::setw(13) << min_v
                      << std::setw(13) << mean
                      << std::setw(13) << p50
                      << std::setw(13) << p99
                      << std::setw(13) << max_v
                      << std::setw(12) << jitter << "\n";
        } else {
            std::cout << std::setw(13) << "-"
                      << std::setw(13) << "-"
                      << std::setw(13) << "-"
                      << std::setw(13) << "-"
                      << std::setw(13) << "-"
                      << std::setw(12) << "-" << "\n";
        }
    }
    std::cout << "=========================================================================================================\n";
    std::cout.flush();
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Uso: sudo " << argv[0] << " <iface_tx> <iface_rx> [arquivo_saida.csv]\n";
        std::cerr << "Exemplo: sudo " << argv[0] << " eth0 eth1 medidas_switch.csv\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const char* tx_iface = argv[1];
    const char* rx_iface = argv[2];
    const std::string csv_filename = (argc >= 4) ? argv[3] : "latency_results.csv";

    const uint16_t udp_port = 319;
    const uint64_t MAGIC_TAG = 0x5357495443485453ULL;

    int tx_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    int rx_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (tx_sock < 0 || rx_sock < 0) {
        perror("socket AF_PACKET (execute com sudo)");
        return EXIT_FAILURE;
    }

    int tx_ifindex = 0, rx_ifindex = 0;
    uint8_t tx_mac[6] {}, rx_mac[6] {};
    get_iface_info(tx_sock, tx_iface, tx_ifindex, tx_mac);
    get_iface_info(rx_sock, rx_iface, rx_ifindex, rx_mac);

    enable_hw_timestamping(tx_sock, tx_iface);
    enable_hw_timestamping(rx_sock, rx_iface);

    int tx_flags = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_OPT_TSONLY;
    setsockopt(tx_sock, SOL_SOCKET, SO_TIMESTAMPING, &tx_flags, sizeof(tx_flags));

    int rx_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    setsockopt(rx_sock, SOL_SOCKET, SO_TIMESTAMPING, &rx_flags, sizeof(rx_flags));

    sockaddr_ll sll_tx {};
    sll_tx.sll_family = AF_PACKET;
    sll_tx.sll_protocol = htons(ETH_P_ALL);
    sll_tx.sll_ifindex = tx_ifindex;
    if (bind(tx_sock, reinterpret_cast<sockaddr*>(&sll_tx), sizeof(sll_tx)) < 0) {
        perror("bind tx_sock");
        return EXIT_FAILURE;
    }

    sockaddr_ll sll_rx {};
    sll_rx.sll_family = AF_PACKET;
    sll_rx.sll_protocol = htons(ETH_P_ALL);
    sll_rx.sll_ifindex = rx_ifindex;
    if (bind(rx_sock, reinterpret_cast<sockaddr*>(&sll_rx), sizeof(sll_rx)) < 0) {
        perror("bind rx_sock");
        return EXIT_FAILURE;
    }

    std::ofstream csv_file(csv_filename);
    if (!csv_file.is_open()) {
        std::cerr << "Erro ao abrir arquivo CSV: " << csv_filename << "\n";
        return EXIT_FAILURE;
    }
    csv_file << "elapsed_sec,seq_id,frame_size_bytes,latency_us\n";

    {
        uint8_t warmup[60] {};
        auto* eth = reinterpret_cast<ether_header*>(warmup);
        std::memcpy(eth->ether_dhost, tx_mac, 6);
        std::memcpy(eth->ether_shost, rx_mac, 6);
        eth->ether_type = htons(0x88B5);
        send(rx_sock, warmup, sizeof(warmup), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::vector<size_t> frame_sizes = {64, 128, 256, 512, 1024, 1518};
    std::unordered_map<size_t, FrameStats> stats_map;
    std::unordered_map<uint64_t, PendingPacket> pending_packets;
    std::mutex mtx;

    auto start_time = std::chrono::steady_clock::now();

    std::thread rx_thread([&]() {
        uint8_t buf[2048];
        char ctrl[1024];
        const size_t min_frame_len = UDP_PAYLOAD_OFFSET + sizeof(PacketPayload);

        while (g_running) {
            iovec iov { buf, sizeof(buf) };
            msghdr msg {};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctrl;
            msg.msg_controllen = sizeof(ctrl);

            ssize_t len = recvmsg(rx_sock, &msg, MSG_DONTWAIT);
            if (len >= static_cast<ssize_t>(min_frame_len)) {
                auto* eth = reinterpret_cast<ether_header*>(buf);
                if (ntohs(eth->ether_type) == ETH_P_IP && std::memcmp(eth->ether_dhost, rx_mac, 6) == 0) {
                    auto* ip = reinterpret_cast<iphdr*>(buf + sizeof(ether_header));
                    if (ip->protocol == IPPROTO_UDP) {
                        PacketPayload payload;
                        std::memcpy(&payload, buf + UDP_PAYLOAD_OFFSET, sizeof(payload));
                        if (payload.magic == MAGIC_TAG) {
                            uint64_t rx_ns_ts = extract_hw_timestamp(&msg);
                            if (rx_ns_ts > 0) {
                                std::lock_guard<std::mutex> lock(mtx);
                                auto it = pending_packets.find(payload.seq_id);
                                if (it != pending_packets.end() && rx_ns_ts > it->second.tx_ns) {
                                    double diff_us = static_cast<double>(rx_ns_ts - it->second.tx_ns) / 1000.0;
                                    size_t fsize = it->second.frame_size;

                                    stats_map[fsize].received++;
                                    stats_map[fsize].latencies_us.push_back(diff_us);

                                    csv_file << std::fixed << std::setprecision(4)
                                             << it->second.elapsed_sec << ","
                                             << payload.seq_id << ","
                                             << fsize << ","
                                             << diff_us << "\n";

                                    pending_packets.erase(it);
                                }
                            }
                        }
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
    });

    std::thread ui_thread([&]() {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!g_running) break;
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            print_dashboard(frame_sizes, stats_map, mtx, elapsed, csv_filename, true);
        }
    });

    std::vector<std::vector<uint8_t>> prebuilt_frames;
    for (size_t frame_size : frame_sizes) {
        size_t wire_buf_len = frame_size - 4;
        std::vector<uint8_t> frame(wire_buf_len, 0x00);

        auto* eth = reinterpret_cast<ether_header*>(frame.data());
        std::memcpy(eth->ether_dhost, rx_mac, 6);
        std::memcpy(eth->ether_shost, tx_mac, 6);
        eth->ether_type = htons(ETH_P_IP);

        auto* ip = reinterpret_cast<iphdr*>(frame.data() + sizeof(ether_header));
        ip->ihl = 5;
        ip->version = 4;
        ip->tos = 0;
        ip->tot_len = htons(static_cast<uint16_t>(wire_buf_len - sizeof(ether_header)));
        ip->id = htons(1234);
        ip->frag_off = 0;
        ip->ttl = 64;
        ip->protocol = IPPROTO_UDP;
        inet_pton(AF_INET, "10.255.255.1", &ip->saddr);
        inet_pton(AF_INET, "10.255.255.2", &ip->daddr);
        ip->check = 0;
        ip->check = ipv4_checksum(ip, sizeof(iphdr));

        auto* udp = reinterpret_cast<udphdr*>(frame.data() + sizeof(ether_header) + sizeof(iphdr));
        udp->source = htons(40000);
        udp->dest = htons(udp_port);
        udp->len = htons(static_cast<uint16_t>(wire_buf_len - sizeof(ether_header) - sizeof(iphdr)));
        udp->check = 0;

        prebuilt_frames.push_back(std::move(frame));
    }

    uint64_t seq_counter = 1;
    size_t size_idx = 0;

    while (g_running) {
        size_t current_frame_size = frame_sizes[size_idx];
        auto& frame = prebuilt_frames[size_idx];
        uint64_t current_seq = seq_counter++;
        const PacketPayload payload{MAGIC_TAG, current_seq};
        std::memcpy(frame.data() + UDP_PAYLOAD_OFFSET, &payload, sizeof(payload));

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();

        if (send(tx_sock, frame.data(), frame.size(), 0) > 0) {
            char ctrl[1024];
            msghdr err_msg {};
            err_msg.msg_control = ctrl;
            err_msg.msg_controllen = sizeof(ctrl);

            int retries = 0;
            while (g_running && recvmsg(tx_sock, &err_msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) {
                if (++retries > 100000) break;
            }

            uint64_t tx_ns_ts = extract_hw_timestamp(&err_msg);
            if (tx_ns_ts > 0) {
                std::lock_guard<std::mutex> lock(mtx);
                stats_map[current_frame_size].sent++;
                pending_packets[current_seq] = { tx_ns_ts, static_cast<uint16_t>(current_frame_size), elapsed };

                if (current_seq > 2000) {
                    pending_packets.erase(current_seq - 2000);
                }
            }
        }

        size_idx = (size_idx + 1) % frame_sizes.size();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rx_thread.join();
    ui_thread.join();

    csv_file.flush();
    csv_file.close();

    auto end_time = std::chrono::steady_clock::now();
    double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    print_dashboard(frame_sizes, stats_map, mtx, total_elapsed, csv_filename, true);
    std::cout << "Dados salvos com sucesso em: " << csv_filename << "\n";

    close(tx_sock);
    close(rx_sock);
    return 0;
}
