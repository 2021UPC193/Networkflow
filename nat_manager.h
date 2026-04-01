#ifndef NAT_MANAGER_H
#define NAT_MANAGER_H

#include <rte_mbuf.h>
#include <stdint.h>

struct nat_manager;
struct nat_session_entry;
struct nat_tuple;

// 初始化NAT管理器
struct nat_manager *nat_manager_init(void);

// 处理数据包
int nat_process_packet(struct rte_mbuf *m, uint16_t port_id, struct nat_manager *mgr);

// 查找会话
struct nat_session_entry *nat_lookup_session(struct nat_manager *mgr, struct nat_tuple *key, int is_outbound);

// 创建会话
struct nat_session_entry *nat_create_session(struct nat_manager *mgr, struct nat_tuple *original, uint32_t external_ip);

// 删除会话
int nat_delete_session(struct nat_manager *mgr, struct nat_session_entry *session);

// 会话老化
void nat_aging_sessions(struct nat_manager *mgr);

// 获取统计信息
void nat_get_stats(struct nat_manager *mgr, uint64_t *sessions, uint64_t *hits, uint64_t *misses);

// 清理NAT管理器
void nat_manager_cleanup(struct nat_manager *mgr);

#endif