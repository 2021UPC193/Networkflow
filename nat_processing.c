#include "nat.h"
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_hash.h>

// 声明全局会话表（在main.c中定义）
extern struct rte_hash *nat_session_table;
static uint16_t external_port_base = 30000; // 外部端口起始范围
#define PORT_RANGE_SIZE 10000

// 生成一个外部端口
static uint16_t allocate_external_port(void) {
    static uint16_t current_port = 0;
    if (current_port == 0) {
        current_port = external_port_base;
    }
    uint16_t port = current_port;
    current_port++;
    if (current_port >= external_port_base + PORT_RANGE_SIZE) {
        current_port = external_port_base;
    }
    return port;
}

// 创建或查找NAT会话
static struct nat_session_entry *create_or_lookup_session(struct nat_tuple *original, int is_outbound, uint32_t external_ip) {
    int ret;
    struct nat_session_entry *entry = NULL;
    struct nat_session_entry new_entry;
    void *data_ptr = NULL;

    // 为新表项分配内存
    struct nat_session_entry *new_entry = NULL;
    if (rte_mempool_get(session_pool, (void **)&new_entry) != 0) {
        printf("Failed to allocate session from pool\n");
        return NULL;
    }
    memset(new_entry, 0, sizeof(*new_entry));

    // 根据方向，决定查询的键
    struct nat_tuple lookup_key;
    if (is_outbound) {
        // 出方向：用原始五元组查找
        memcpy(&lookup_key, original, sizeof(struct nat_tuple));
    } else {
        // 入方向：理论上需要用转换后的五元组反向查找，这里简化处理
        // 一个完整的实现需要维护反向映射表。此处仅为示例逻辑。
        memcpy(&lookup_key, original, sizeof(struct nat_tuple));
        // 实际应将目的IP和端口与外部IP和端口交换后查找
    }

    ret = rte_hash_lookup_data(nat_session_table, &lookup_key, &data_ptr);
    if (ret >= 0 && data_ptr != NULL) {
        // 找到现有会话
        entry = (struct nat_session_entry *)data_ptr;
        entry->aging = 0; // 重置老化计数器
        return entry;
    }

    // 未找到，创建新会话（仅处理出方向）
    if (is_outbound) {
        memset(&new_entry, 0, sizeof(new_entry));
        memcpy(&new_entry.original, original, sizeof(struct nat_tuple));

        // 构建转换后的五元组
        new_entry.translated.src_ip = external_ip; // 替换为外部IP
        new_entry.translated.dst_ip = original->dst_ip;
        new_entry.translated.src_port = allocate_external_port(); // 分配新端口
        new_entry.translated.dst_port = original->dst_port;
        new_entry.translated.protocol = original->protocol;

        new_entry.create_time = rte_rdtsc();
        new_entry.nat_type = NAT_TYPE_FULL_CONE; // 示例：全锥型NAT
        new_entry.aging = 0;

        // 将会话插入哈希表（以原始元组为键）
        int key_idx = rte_hash_add_key_data(nat_session_table, &new_entry.original, &new_entry);
        if (key_idx < 0) {
            printf("Failed to add session to hash table\n");
            return NULL;
        }
        // 此处也应插入以转换后元组为键的反向条目以便入方向查询
        // rte_hash_add_key_data(reverse_table, &new_entry.translated, &new_entry);
        entry = &new_entry;
    }
    return entry;
}

// 核心数据包处理函数
int process_packet(struct rte_mbuf *m, uint16_t port_id) {
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr = NULL;
    struct rte_tcp_hdr *tcp_hdr = NULL;
    struct nat_tuple tuple;
    struct nat_session_entry *session = NULL;
    uint32_t external_ip = 0; // 应配置为WAN口IP
    int is_outbound = 1;      // 简化：假设port_id 0是内网口，1是外网口
    uint16_t l3_len, l4_len = 0;

    // 1. 解析以太网头部
    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    uint16_t ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);
    if (ether_type != RTE_ETHER_TYPE_IPV4) {
        return -1; // 非IPv4，不支持
    }

    // 2. 解析IPv4头部
    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    if (ip_hdr->next_proto_id != IPPROTO_UDP && ip_hdr->next_proto_id != IPPROTO_TCP) {
        return -1; // 非TCP/UDP，不支持
    }

    // 3. 构建NAT元组键
    memset(&tuple, 0, sizeof(tuple));
    tuple.src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
    tuple.dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
    tuple.protocol = ip_hdr->next_proto_id;

    l3_len = (ip_hdr->version_ihl & 0x0f) * 4;

    // 4. 解析传输层头部，获取端口
    if (tuple.protocol == IPPROTO_UDP) {
        udp_hdr = (struct rte_udp_hdr *)((char *)ip_hdr + l3_len);
        tuple.src_port = rte_be_to_cpu_16(udp_hdr->src_port);
        tuple.dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);
        l4_len = sizeof(struct rte_udp_hdr);
    } else if (tuple.protocol == IPPROTO_TCP) {
        tcp_hdr = (struct rte_tcp_hdr *)((char *)ip_hdr + l3_len);
        tuple.src_port = rte_be_to_cpu_16(tcp_hdr->src_port);
        tuple.dst_port = rte_be_to_cpu_16(tcp_hdr->dst_port);
        l4_len = ((tcp_hdr->data_off & 0xf0) >> 4) * 4;
    } else {
        return -1;
    }

    // 5. 确定方向并查找/创建会话
    // 此处简化：假设端口0是私有网络，端口1是公共网络
    is_outbound = (port_id == 0);
    // 设置外部IP（应通过配置获取，这里示例）
    external_ip = (is_outbound) ? rte_be_to_cpu_32(rte_cpu_to_be_32(0xDEADBEEF)) : 0; // 示例IP

    session = create_or_lookup_session(&tuple, is_outbound, external_ip);
    if (!session) {
        return -1; // 创建或查找会话失败
    }

    // 6. 修改数据包
    if (is_outbound) {
        // 出方向：修改源IP和源端口
        ip_hdr->src_addr = rte_cpu_to_be_32(session->translated.src_ip);
        if (udp_hdr) {
            udp_hdr->src_port = rte_cpu_to_be_16(session->translated.src_port);
            // 重新计算UDP校验码（可选，DPDK网卡可能硬件卸载）
        } else if (tcp_hdr) {
            tcp_hdr->src_port = rte_cpu_to_be_16(session->translated.src_port);
            // 重新计算TCP校验码
        }
    } else {
        // 入方向：修改目的IP和目的端口
        ip_hdr->dst_addr = rte_cpu_to_be_32(session->original.src_ip);
        if (udp_hdr) {
            udp_hdr->dst_port = rte_cpu_to_be_16(session->original.src_port);
        } else if (tcp_hdr) {
            tcp_hdr->dst_port = rte_cpu_to_be_16(session->original.src_port);
        }
    }
    // 重要：重新计算IP头部校验和
    ip_hdr->hdr_checksum = 0;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

    // 7. 更新MAC地址（需要根据ARP表设置，此处为简化，实际需查询下一跳MAC）
    // 省略ARP解析步骤，仅示意
    // rte_ether_addr_copy(&next_hop_mac, &eth_hdr->s_addr);
    // rte_ether_addr_copy(&port_mac, &eth_hdr->d_addr);

    return 0;
}

// 会话老化函数（由控制核心调用）
void aging_sessions(void) {
    uint32_t iter = 0;
    void *key, *data;
    struct nat_session_entry *entry;
    uint64_t now = rte_rdtsc();
    uint64_t timeout_cycles = SESSION_TIMEOUT * rte_get_timer_hz();

    // 遍历哈希表
    while (rte_hash_iterate(nat_session_table, &key, &data, &iter) >= 0) {
        entry = (struct nat_session_entry *)data;
        // 超过设定时间则删除
        if (now - entry->create_time > timeout_cycles) {
            rte_hash_del_key(nat_session_table, key);
            // 也需要从反向表中删除
            // rte_hash_del_key(reverse_table, &entry->translated);
            printf("Aged out session\n");
        }
    }
}