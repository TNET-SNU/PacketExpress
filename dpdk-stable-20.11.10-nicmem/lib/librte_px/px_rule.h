#ifndef PX_RULE_H_
#define PX_RULE_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
/*----------------------------------------------------------------------------*/
#define MAX_PATTERN_NUM 8
#define MAX_ACTION_NUM 8
#define MAX_PX_SET_NUM 8
#define MAX_FIELD_NUM 8
#define ETH_HDR_ZEROS ((uint8_t[sizeof(struct rte_ether_hdr)]){0})
#define IPV4_HDR_ZEROS ((uint8_t[sizeof(struct rte_ipv4_hdr)]){0})
#define TCP_HDR_ZEROS ((uint8_t[sizeof(struct rte_tcp_hdr)]){0})
#define UDP_HDR_ZEROS ((uint8_t[sizeof(struct rte_udp_hdr)]){0})
#define GTP_HDR_ZEROS ((uint8_t[sizeof(struct rte_gtp_hdr)]){0}) // GTPv1-U base header
#define GTP_PSC_HDR_ZEROS ((uint8_t[3]){0})                      // PDU Session Container (PSC)
#define PX_ACTION_TYPE_CONCAT (RTE_FLOW_ACTION_TYPE_END - 1)
/*----------------------------------------------------------------------------*/
struct px_pattern
{
    int type; /* packet express pattern type */
    const void *mask;
};

struct px_action
{
    int type;         /* packet express action type */
    const void *conf; /* Pointer to action configuration object. */
};

struct px_mask_vec
{
    uint8_t off;
    uint8_t len;
    uint8_t val[16];
};

struct px_pattern_types
{
    /* ToDo: extend this structure to support various cases */
    uint16_t l2_type;
    rte_be16_t ether_type;
    uint8_t proto_id;
    rte_be16_t src_port;
    rte_be16_t dst_port;
};

/* decoded rules */
struct decoded_pattern
{
    struct px_mask_vec *vec;
    struct px_pattern_types types;
    uint16_t nb_chunks;
    uint16_t use_rss_hash;
};

struct decoded_action
{
    uint16_t do_concat : 1;
    uint16_t decap_size;
};

struct px_rule
{
    /* rule identifier */
    uint16_t px_id;

    /* copied rules */

    /* patterns for this flow */
    struct px_pattern patterns[MAX_PATTERN_NUM];
    uint16_t nb_patterns;

    /* actions for this flow */
    struct px_action actions[MAX_ACTION_NUM];
    uint16_t nb_actions;

    /* decoded rules */
    struct decoded_pattern dec_pat;
    struct decoded_action dec_act;

    TAILQ_ENTRY(px_rule)
    link; /* px_rule entry link */
};

typedef TAILQ_HEAD(queue_head, px_rule) queue_head;
/*----------------------------------------------------------------------------*/
struct px_rule_set
{
    /* pattern matching rules */
    queue_head rules;
    uint16_t rule_num;
};
/*----------------------------------------------------------------------------*/
void rule_init(uint16_t port_id,
               uint16_t queue_id);
/*----------------------------------------------------------------------------*/
int rule_insert(uint16_t port_id,
                uint16_t queue_id,
                struct px_rule *rule);
/*----------------------------------------------------------------------------*/
void rule_remove(uint16_t port_id,
                 uint16_t queue_id,
                 struct px_rule *rule);
/*----------------------------------------------------------------------------*/
struct px_rule *
rule_search(uint16_t port_id,
            uint16_t queue_id,
            int px_id);
/*----------------------------------------------------------------------------*/
int use_rss_hash(struct px_pattern *patterns,
                 int nb_patterns);
/*----------------------------------------------------------------------------*/
int pattern_to_hdr_size(struct px_pattern *pattern);
/*----------------------------------------------------------------------------*/
int patterns_to_mask_offset(struct px_pattern *patterns,
                            int nb_patterns);
/*----------------------------------------------------------------------------*/
int patterns_to_types(struct px_pattern *patterns,
                      int nb_patterns,
                      struct px_pattern_types *types);
/*----------------------------------------------------------------------------*/
int patterns_l3_type(struct px_pattern *patterns,
                     int nb_patterns);
/*----------------------------------------------------------------------------*/
struct px_mask_vec *
patterns_to_vec(struct px_pattern *patterns,
                uint16_t nb_patterns,
                uint16_t *nb_chunks);
/*----------------------------------------------------------------------------*/
int decode_pattern_rule(struct decoded_pattern *rule,
                        struct px_pattern *patterns,
                        uint16_t nb_patterns);
/*----------------------------------------------------------------------------*/
int decode_action_rule(struct decoded_action *rule,
                       struct px_action *actions,
                       uint16_t nb_actions);
/*----------------------------------------------------------------------------*/
int px_rule_add(uint16_t port_id,
                uint16_t queue_id,
                struct px_pattern *patterns,
                int nb_patterns,
                struct px_action *actions,
                int nb_actions);
/*----------------------------------------------------------------------------*/
void px_rule_del(uint16_t port_id,
                 uint16_t queue_id,
                 int px_id);
/*----------------------------------------------------------------------------*/
void px_del_rule_all(uint16_t port_id,
                     uint16_t queue_id);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_RULE_H_