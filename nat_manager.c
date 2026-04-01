#include "nat_manager.h"
#include "nat.h"
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_hash.h>
#include <rte_mempool.h>
#include <rte_atomic.h>
#include <rte_jhash.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <string.h>
#include <stdio.h>
#include <rte_log.h>

RTE_LOG_REGISTER_DEFAULT(nat_logtype, INFO);

#define RTE_LOGTYPE_NAT nat_logtype

// NAT管理器结构
struct nat_manager {
    // 原始→会话
    struct rte_hash *forward_table;
    // 转换后→会话
    struct rte_hash *reverse_table;
    
    // 内存池
    struct rte_mempool *session_pool;
    
    // 端口管理
    struct {
        rte_atomic16_t current_port;
        uint16_t base_port;
        uint16_t range_size;
    } port_mgr;
    
    // 配置
    struct {
        uint32_t external_ip;
        uint8_t internal_port;
        uint8_t external_port;
        uint64_t session_timeout;  // 超时时间（秒）
    } config;
    
    // 统计
    struct {
        rte_atomic64_t sessions_created;
        rte_atomic64_t sessions_deleted;
        rte_atomic64_t lookups;
        rte_atomic64_t hits;
        rte_atomic64_t misses;
        rte_atomic64_t collisions;
    } stats;
};

// 初始化NAT管理器
struct nat_manager *nat_manager_init(void) {
    struct nat_manager *mgr = rte_zmalloc("nat_manager", sizeof(struct nat_manager), 0);
    if (!mgr) {
        rte_log(RTE_LOG_ERR, RTE_LOGTYPE_NAT, "Failed to allocate NAT manager\n");
        return NULL;
    }
    
    // 初始化端口管理器
    mgr->port_mgr.base_port = 30000;
    mgr->port_mgr.range_size = 10000;
    rte_atomic16_init(&mgr->port_mgr.current_port);
    
    // 创建内存池
    char pool_name[32];
    snprintf(pool_name, sizeof(pool_name), "nat_session_pool_%d", rte_socket_id());
    
    mgr->session_pool = rte_mempool_create(pool_name,
                                           MAX_SESSIONS,
                                           sizeof(struct nat_session_entry),
                                           1024,
                                           0,
                                           NULL, NULL, NULL, NULL,
                                           rte_socket_id(), 0);
    if (!mgr->session_pool) {
        RTE_LOG(ERR, NAT, "Failed to create session pool\n");
        rte_free(mgr);
        return NULL;
    }
    
    // 创建正向哈希表
    char hash_name[32];
    snprintf(hash_name, sizeof(hash_name), "nat_forward_table_%d", rte_socket_id());
    
    struct rte_hash_parameters forward_params = {
        .name = hash_name,
        .entries = MAX_SESSIONS,
        .key_len = sizeof(struct nat_tuple),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                     RTE_HASH_EXTRA_FLAGS_TRANS_MEM_SUPPORT,
    };
    
    mgr->forward_table = rte_hash_create(&forward_params);
    if (!mgr->forward_table) {
        RTE_LOG(ERR, NAT, "Failed to create forward hash table\n");
        rte_mempool_free(mgr->session_pool);
        rte_free(mgr);
        return NULL;
    }
    
    // 创建反向哈希表
    snprintf(hash_name, sizeof(hash_name), "nat_reverse_table_%d", rte_socket_id());
    forward_params.name = hash_name;
    
    mgr->reverse_table = rte_hash_create(&forward_params);
    if (!mgr->reverse_table) {
        RTE_LOG(ERR, NAT, "Failed to create reverse hash table\n");
        rte_hash_free(mgr->forward_table);
        rte_mempool_free(mgr->session_pool);
        rte_free(mgr);
        return NULL;
    }
    
    // 初始化统计
    rte_atomic64_init(&mgr->stats.sessions_created);
    rte_atomic64_init(&mgr->stats.sessions_deleted);
    rte_atomic64_init(&mgr->stats.lookups);
    rte_atomic64_init(&mgr->stats.hits);
    rte_atomic64_init(&mgr->stats.misses);
    rte_atomic64_init(&mgr->stats.collisions);
    
    // 默认配置
    mgr->config.session_timeout = 300;  // 5分钟
    
    RTE_LOG(INFO, NAT, "NAT manager initialized successfully\n");
    return mgr;
}

// 分配外部端口
static uint16_t allocate_external_port(struct nat_manager *mgr) {
    uint16_t port = rte_atomic16_add_return(&mgr->port_mgr.current_port, 1);
    
    if (port >= mgr->port_mgr.base_port + mgr->port_mgr.range_size) {
        rte_atomic16_set(&mgr->port_mgr.current_port, mgr->port_mgr.base_port);
        port = mgr->port_mgr.base_port;
    }
    
    return port;
}

// 查找会话
struct nat_session_entry *nat_lookup_session(struct nat_manager *mgr,
                                            struct nat_tuple *key,
                                            int is_outbound) {
    rte_atomic64_inc(&mgr->stats.lookups);
    
    struct nat_session_entry *entry = NULL;
    int ret;
    
    if (is_outbound) {
        ret = rte_hash_lookup_data(mgr->forward_table, key, (void **)&entry);
    } else {
        ret = rte_hash_lookup_data(mgr->reverse_table, key, (void **)&entry);
    }
    
    if (ret >= 0 && entry) {
        rte_atomic64_inc(&mgr->stats.hits);
        rte_atomic32_inc(&entry->refcnt);
        return entry;
    }
    
    rte_atomic64_inc(&mgr->stats.misses);
    return NULL;
}

// 创建会话
struct nat_session_entry *nat_create_session(struct nat_manager *mgr,
                                            struct nat_tuple *original,
                                            uint32_t external_ip) {
    // 分配会话内存
    struct nat_session_entry *session = NULL;
    if (rte_mempool_get(mgr->session_pool, (void **)&session) != 0) {
        RTE_LOG(ERR, NAT, "Failed to allocate session from pool\n");
        return NULL;
    }
    
    // 初始化会话
    memset(session, 0, sizeof(*session));
    memcpy(&session->original, original, sizeof(struct nat_tuple));
    
    // 构建转换后的五元组
    session->translated.src_ip = external_ip;
    session->translated.dst_ip = original->dst_ip;
    session->translated.src_port = allocate_external_port(mgr);
    session->translated.dst_port = original->dst_port;
    session->translated.protocol = original->protocol;
    
    session->create_time = rte_rdtsc();
    session->nat_type = NAT_TYPE_FULL_CONE;
    session->refcnt = 1;
    
    // 插入正向表
    int ret1 = rte_hash_add_key_data(mgr->forward_table, 
                                    &session->original, session);
    if (ret1 < 0) {
        RTE_LOG(ERR, NAT, "Failed to add session to forward table: %d\n", ret1);
        rte_mempool_put(mgr->session_pool, session);
        return NULL;
    }
    
    // 插入反向表
    int ret2 = rte_hash_add_key_data(mgr->reverse_table,
                                    &session->translated, session);
    if (ret2 < 0) {
        RTE_LOG(ERR, NAT, "Failed to add session to reverse table: %d\n", ret2);
        rte_hash_del_key(mgr->forward_table, &session->original);
        rte_mempool_put(mgr->session_pool, session);
        return NULL;
    }
    
    rte_atomic64_inc(&mgr->stats.sessions_created);
    return session;
}

// 删除会话
int nat_delete_session(struct nat_manager *mgr,
                      struct nat_session_entry *session) {

     // --- 新增：安全检查，确保没有正在使用的引用 ---
    // 注意：在调用此函数前，应已通过`nat_session_put`或类似机制使refcnt归零。
    // 此处使用assert帮助调试，生产环境可改为日志警告并返回错误。
    if (session->refcnt != 0) {
        RTE_LOG(WARNING, NAT, "Attempting to delete a session (orig: %u.%u.%u.%u:%u) with refcnt=%u\n",
                (session->original.src_ip >> 24) & 0xFF,
                (session->original.src_ip >> 16) & 0xFF,
                (session->original.src_ip >> 8) & 0xFF,
                session->original.src_ip & 0xFF,
                session->original.src_port,
                session->refcnt);
        return -1;
    }

    int ret1 = rte_hash_del_key(mgr->forward_table, &session->original);
    int ret2 = rte_hash_del_key(mgr->reverse_table, &session->translated);
    
    if (ret1 < 0 || ret2 < 0) {
        RTE_LOG(WARNING, NAT, "Failed to delete session from tables\n");
        return -1;
    }
    
    rte_mempool_put(mgr->session_pool, session);
    rte_atomic64_inc(&mgr->stats.sessions_deleted);
    
    return 0;
}

// 会话老化
void nat_aging_sessions(struct nat_manager *mgr) {
    uint32_t iter = 0;
    const void *key;
    void *data;
    uint64_t now = rte_rdtsc();
    uint64_t timeout_cycles = mgr->config.session_timeout * rte_get_timer_hz();
    
    // 收集过期会话
    struct nat_session_entry *expired_sessions[1024];
    int expired_count = 0;
    
    while (rte_hash_iterate(mgr->forward_table, &key, &data, &iter) >= 0) {
        struct nat_session_entry *session = (struct nat_session_entry *)data;
        
        if (now - session->create_time > timeout_cycles) {
            uint32_t ref_before = rte_atomic32_read(&session->refcnt);
            if (ref_before == 0) {
                // 引用已为0，可以安全收集删除
                if (expired_count < 1024) {
                    expired_sessions[expired_count++] = session;
                } else {
                    RTE_LOG(WARNING, NAT, "Too many expired sessions, breaking\n");
                    break;
                }
            } else {
                // 引用计数 > 0，说明仍有数据核心在使用，本次老化跳过。
                RTE_LOG(DEBUG, NAT, "Session (orig port: %u) still in use (refcnt=%u), skip aging this cycle.\n",
                        session->original.src_port, ref_before);
            }
        }
    }
    
    // 批量删除
    for (int i = 0; i < expired_count; i++) {
        nat_delete_session(mgr, expired_sessions[i]);
    }
    
    if (expired_count > 0) {
        RTE_LOG(DEBUG, NAT, "Aged out %d sessions\n", expired_count);
    }
}

// 获取统计信息
void nat_get_stats(struct nat_manager *mgr, 
                   uint64_t *sessions, uint64_t *hits, uint64_t *misses) {
    if (sessions) {
        *sessions = rte_atomic64_read(&mgr->stats.sessions_created) -
                   rte_atomic64_read(&mgr->stats.sessions_deleted);
    }
    if (hits) {
        *hits = rte_atomic64_read(&mgr->stats.hits);
    }
    if (misses) {
        *misses = rte_atomic64_read(&mgr->stats.misses);
    }
}

// 清理NAT管理器
void nat_manager_cleanup(struct nat_manager *mgr) {
    if (!mgr) return;
    
    // 删除所有会话
    uint32_t iter = 0;
    const void *key;
    void *data;
    
    while (rte_hash_iterate(mgr->forward_table, &key, &data, &iter) >= 0) {
        struct nat_session_entry *session = (struct nat_session_entry *)data;
        rte_mempool_put(mgr->session_pool, session);
    }
    
    // 释放哈希表
    rte_hash_free(mgr->forward_table);
    rte_hash_free(mgr->reverse_table);
    
    // 释放内存池
    rte_mempool_free(mgr->session_pool);
    
    // 释放管理器
    rte_free(mgr);
    
    RTE_LOG(INFO, NAT, "NAT manager cleaned up\n");
}

// 释放对会话的引用。当引用计数减为0时，实际删除会话。
int nat_session_put(struct nat_manager *mgr, struct nat_session_entry *session) {
    if (!session || !mgr) {
        return -1;
    }
    
    // 减少引用计数
    uint32_t new_ref = rte_atomic32_sub_return(&session->refcnt, 1);
    
    // 如果引用计数减到0，说明没有任何数据核心再持有此会话，可以安全删除
    if (new_ref == 0) {
        // 这里可以将会话放入一个待删除队列，由控制核心统一清理，
        // 或者直接删除（需确保调用者不在数据平面的快速路径上）。
        // 为简化，此处直接调用删除。更优解是使用rte_ring异步通知控制核心。
        nat_delete_session(mgr, session);
    }
    return 0;
}