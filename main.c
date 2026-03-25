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
#define MAX_SESSIONS (1024 * 1024)

// NAT会话状态表
struct rte_hash *nat_session_table = NULL;
static volatile int force_quit = 0;

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


/*
 * “lcore 主线程”。这是执行工作的主要线程，它从输入端口读取数据，并将数据写入输出端口。
*/
static void
lcore_main(void)
{
	uint16_t port = 0;
	uint64_t prev_tsc = rte_get_timer_cycles();
	uint64_t cur_tsc;
    uint64_t hz = rte_get_timer_hz();
    uint64_t interval_tsc = hz;  /* 1秒的时钟周期数 */
	

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());

	while (!force_quit) {
		/* 从一对端口中的第一个端口获取一波接收数据包 */
		struct rte_mbuf *bufs[BURST_SIZE];
		const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
        struct rte_mempool *p = rte_pktmbuf_pool_create()
	}

}


int
main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint16_t portid = 0;

	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	argc -= ret;
	argv += ret;

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 1)
		rte_exit(EXIT_FAILURE, "Error: no enough ports available\n");

	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	if (port_init(portid, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n", portid);

	if (rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	lcore_main();

	rte_eth_dev_stop(portid);
    rte_eth_dev_close(portid);
	rte_eal_cleanup();

	return 0;
}
