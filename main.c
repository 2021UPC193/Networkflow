#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <signal.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_net.h>
#include <rte_hash.h>
#include <nat.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

// 全局数据结构
struct rte_hash *nat_session_table = NULL;
static volatile int force_quit = 0;
struct rte_mempool *mbuf_pool = NULL;
uint16_t port_count = 0;
uint16_t ports[RTE_MAX_ETHPORTS];

// 为每个逻辑核心定义处理上下文
struct lcore_context {
    uint16_t port_id; // 该核心处理的端口
    uint64_t processed_pkts;
} __rte_cache_aligned;
struct lcore_context lcore_ctx[RTE_MAX_LCORE];

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        force_quit = 1;
    }
}

static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error during getting device (port %u) info: %s\n", port, strerror(-retval));
		return retval;
	}

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	/* 配置网卡 */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* 为port分配一个接收队列. */
	retval = rte_eth_rx_queue_setup(port, 0, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
	if (retval < 0)
		return retval;
	
	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;

	/* 为port分配一个发送队列. */
	retval = rte_eth_tx_queue_setup(port, 0, nb_txd, rte_eth_dev_socket_id(port), &txconf);
	if (retval < 0)
		return retval;

	/* 启动port. 8< */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* 显示端口的MAC地址. */
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port, RTE_ETHER_ADDR_BYTES(&addr));

	/* 在混杂模式下启用以太网设备的接收功能 */
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

static inline int 
init_nat_table(void) {
    struct rte_hash_parameters hash_params = {
        .name = "nat_session",
        .entries = MAX_SESSIONS,                // 最大条目数
        .key_len = sizeof(struct nat_tuple),    // 键长度
        .hash_func = rte_hash_crc,              // 使用CRC硬件加速（如果CPU支持）
        .hash_func_init_val = 0,                // 随机种子，避免hash碰撞
        .socket_id = rte_socket_id(),           // 绑定到当前NUMA节点
    };
    nat_session_table = rte_hash_create(&hash_params);
    if (!nat_session_table) {
        rte_exit(EXIT_FAILURE, "Failed to create NAT hash table\n");
    }
    return 0;
}

// NAT表初始化函数 (同您提供的代码，此处略)
static inline int init_nat_table(void) {
    // ... [保持您原有的 init_nat_table 函数代码完全不变] ...
}

// 数据平面核心的主处理函数
static int lcore_main_loop(void *arg) {
    uint16_t lcore_id = rte_lcore_id();
    struct lcore_context *ctx = &lcore_ctx[lcore_id];
    uint16_t port_id = ctx->port_id;
    struct rte_mbuf *bufs[BURST_SIZE];
    int nb_rx, i;

    printf("Data plane core %u started, processing port %u\n", lcore_id, port_id);

    while (!force_quit) {
        // 1. 接收数据包
        nb_rx = rte_eth_rx_burst(port_id, 0, bufs, BURST_SIZE);
        if (unlikely(nb_rx == 0)) {
            continue;
        }

        // 2. 处理每个数据包 (NAT转换)
        for (i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = bufs[i];
            // 调用NAT处理函数（详见下方 nat_processing.c）
            int ret = process_packet(m, port_id);
            if (unlikely(ret < 0)) {
                // 处理失败，丢弃包
                rte_pktmbuf_free(m);
                bufs[i] = NULL; // 标记为不发送
                nb_rx--;
            }
        }

        // 3. 发送处理后的数据包
        if (nb_rx > 0) {
            uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, bufs, nb_rx);
            ctx->processed_pkts += nb_tx;

            // 释放未成功发送的数据包 (如果发生拥塞)
            if (unlikely(nb_tx < nb_rx)) {
                for (i = nb_tx; i < nb_rx; i++) {
                    rte_pktmbuf_free(bufs[i]);
                }
            }
        }
    }
    return 0;
}

// 控制平面核心：会话表老化任务
static int session_aging_loop(__rte_unused void *arg) {
    const uint64_t hz = rte_get_timer_hz();
    uint64_t prev_tsc = 0, cur_tsc, diff_tsc;

    printf("Control plane core (aging) started on lcore %u\n", rte_lcore_id());

    while (!force_quit) {
        cur_tsc = rte_rdtsc();
        diff_tsc = cur_tsc - prev_tsc;

        // 定时执行老化检查（例如每秒一次）
        if (diff_tsc > AGING_INTERVAL * hz / 1000000) {
            aging_sessions();
            prev_tsc = cur_tsc;
        }
        rte_pause();
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int ret;
    unsigned int lcore_id, master_lcore_id, worker_lcore_count = 0;
    uint16_t port_id;

    // 初始化EAL
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    argc -= ret;
    argv += ret;

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 检查可用端口
    port_count = rte_eth_dev_count_avail();
	// 至少两个端口一个用于数据平面，一个用于控制平面
    if (port_count < 2)
        rte_exit(EXIT_FAILURE, "Error: Need at least 2 available ports. Found %u\n", port_count);
    printf("Number of available ports: %u\n", port_count);

    // 获取所有可用端口ID
    uint16_t port_index = 0;
    RTE_ETH_FOREACH_DEV(port_id) {
        ports[port_index++] = port_id;
    }

    // 创建内存池
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * port_count, MBUF_CACHE_SIZE, 0, 
										RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    // 初始化所有端口
    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_init(port_id, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", port_id);
    }

    // 初始化NAT会话表
    if (init_nat_table() != 0) {
        rte_exit(EXIT_FAILURE, "Cannot init NAT session table\n");
    }

    // --- 核心任务分配逻辑 ---
    master_lcore_id = rte_get_main_lcore();
    printf("Master (control) lcore ID: %u\n", master_lcore_id);

    // 为第一个可用的从核心分配端口0，第二个分配端口1...
    uint16_t assigned_port_idx = 0;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (worker_lcore_count >= 3)
            break;
        if (assigned_port_idx >= port_count) 
            rte_exit(EXIT_FAILURE, "More data cores than available ports.\n");

        lcore_ctx[lcore_id].port_id = ports[assigned_port_idx];
        lcore_ctx[lcore_id].processed_pkts = 0;
        printf("Assigning port %u to worker lcore %u\n", ports[assigned_port_idx], lcore_id);

        assigned_port_idx++;
        worker_lcore_count++;
    }

    if (worker_lcore_count < 3) {
        printf("Warning: Only %u worker lcores available. Using all.\n", worker_lcore_count);
    }

    // 启动数据平面核心（从核心）
    assigned_port_idx = 0;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (assigned_port_idx >= worker_lcore_count) {
            break;
        }
        rte_eal_remote_launch(lcore_main_loop, NULL, lcore_id);
        assigned_port_idx++;
    }

    // 主核心（控制核心）执行会话老化任务
    session_aging_loop(NULL);

    // 等待所有数据平面核心结束
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0) {
            printf("Worker lcore %u exited with error\n", lcore_id);
        } else {
            printf("Worker lcore %u finished, processed %" PRIu64 " packets\n",
                   lcore_id, lcore_ctx[lcore_id].processed_pkts);
        }
    }

    // 清理阶段
    printf("\nCleaning up...\n");
    for (port_id = 0; port_id < port_count; port_id++) {
        rte_eth_dev_stop(port_id);
        rte_eth_dev_close(port_id);
    }
    rte_hash_free(nat_session_table);
    rte_eal_cleanup();
    return 0;
}