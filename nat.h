#include <rte_hash.h>
#include <rte_hash_crc.h>

#define NAT_TYPE_FULL_CONE  0x01
#define NAT_TYPE_RESTRICTED 0x02
#define NAT_TYPE_PORT_REST  0x03
#define NAT_TYPE_SYMMETRIC  0x04

#define MAX_SESSIONS (1024 * 1024)      // 最大会话数
#define SESSION_TIMEOUT 30              // 会话超时时间（秒）
#define AGING_INTERVAL 1000000          // 老化检查间隔（微秒）
#define SESSION_TIMEOUT 30              // 会话超时时间（秒）

struct nat_tuple {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint8_t  padding[3];                // 对齐
} __attribute__((packed));

/* 会话表项 */
struct nat_session_entry {
    struct nat_tuple original;   // 原始五元组
    struct nat_tuple translated; // 转换后的五元组
    uint64_t create_time;        // 创建时间戳 
    uint32_t state;              // TCP状态机
    uint32_t refcnt;             // 引用计数（如果支持共享）
    uint16_t flags;              // 标志位
    uint8_t  nat_type;           // NAT类型：全锥、受限锥、端口受限、对称
    uint8_t  aging;              // 老化计数或标志
} __attribute__((aligned(64)));
