/* ============================================================================
 * PD-Kernel  —  ARP layer  (Phase 14a)
 * ============================================================================ */

#include "arp.h"
#include "net.h"
#include "rtl8139.h"
#include "pit.h"

/* ---- Ethernet + ARP frame layout constants ------------------------------- */
#define ETH_DST      0   /* destination MAC (6 bytes)  */
#define ETH_SRC      6   /* source MAC      (6 bytes)  */
#define ETH_TYPE    12   /* EtherType       (2 bytes)  */
#define ETH_HDR     14   /* size of Ethernet header    */

#define ARP_HTYPE   14   /* hardware type   (2 bytes)  */
#define ARP_PTYPE   16   /* protocol type   (2 bytes)  */
#define ARP_HLEN    18   /* hardware len    (1 byte)   */
#define ARP_PLEN    19   /* protocol len    (1 byte)   */
#define ARP_OPER    20   /* operation       (2 bytes)  */
#define ARP_SHA     22   /* sender hw addr  (6 bytes)  */
#define ARP_SPA     28   /* sender proto addr (4 bytes)*/
#define ARP_THA     32   /* target hw addr  (6 bytes)  */
#define ARP_TPA     38   /* target proto addr (4 bytes)*/
#define ARP_FRAME   42   /* total frame size           */

/* ---- ARP table ----------------------------------------------------------- */
#define ARP_TABLE_SIZE  16

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_entry_t;

static arp_entry_t g_arp_table[ARP_TABLE_SIZE];
static uint8_t     g_our_mac[6];

/* ---- Helpers ------------------------------------------------------------- */

static void write_mac(uint8_t *dst, const uint8_t *src)
{
    int i;
    for (i = 0; i < 6; i++) dst[i] = src[i];
}

static void write_ip(uint8_t *dst, uint32_t ip)
{
    /* store big-endian */
    dst[0] = (uint8_t)(ip >> 24);
    dst[1] = (uint8_t)(ip >> 16);
    dst[2] = (uint8_t)(ip >>  8);
    dst[3] = (uint8_t)(ip      );
}

static uint32_t read_ip(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16)
         | ((uint32_t)src[2] <<  8) |  (uint32_t)src[3];
}

/* ---- Table management ---------------------------------------------------- */

static void arp_table_set(uint32_t ip, const uint8_t *mac)
{
    int i;
    /* Update existing entry if present */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp_table[i].valid && g_arp_table[i].ip == ip) {
            write_mac(g_arp_table[i].mac, mac);
            return;
        }
    }
    /* Find empty slot */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!g_arp_table[i].valid) {
            g_arp_table[i].ip    = ip;
            g_arp_table[i].valid = 1;
            write_mac(g_arp_table[i].mac, mac);
            return;
        }
    }
    /* Table full: evict slot 0 (simple round-robin would be better,
     * but we only have 16 peers in practice) */
    g_arp_table[0].ip    = ip;
    g_arp_table[0].valid = 1;
    write_mac(g_arp_table[0].mac, mac);
}

static int arp_table_get(uint32_t ip, uint8_t mac_out[6])
{
    int i;
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp_table[i].valid && g_arp_table[i].ip == ip) {
            write_mac(mac_out, g_arp_table[i].mac);
            return 1;
        }
    }
    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

void arp_init(void)
{
    int i;
    for (i = 0; i < ARP_TABLE_SIZE; i++)
        g_arp_table[i].valid = 0;

    rtl8139_get_mac(g_our_mac);

    /* Seed the gateway: send a request immediately so the reply arrives
     * before the first outbound IP packet needs to be sent.               */
    arp_send_request(NET_IP_GW);
}

void arp_send_request(uint32_t target_ip)
{
    uint8_t frame[ARP_FRAME];
    int     i;
    for (i = 0; i < ARP_FRAME; i++) frame[i] = 0;

    /* Ethernet header — broadcast destination */
    for (i = 0; i < 6; i++) frame[ETH_DST + i] = 0xFFu;
    write_mac(frame + ETH_SRC, g_our_mac);
    frame[ETH_TYPE]     = (uint8_t)(ETHERTYPE_ARP >> 8);
    frame[ETH_TYPE + 1] = (uint8_t)(ETHERTYPE_ARP);

    /* ARP payload */
    frame[ARP_HTYPE]     = 0x00; frame[ARP_HTYPE + 1] = 0x01; /* Ethernet */
    frame[ARP_PTYPE]     = 0x08; frame[ARP_PTYPE + 1] = 0x00; /* IPv4     */
    frame[ARP_HLEN]      = 6;
    frame[ARP_PLEN]      = 4;
    frame[ARP_OPER]      = 0x00; frame[ARP_OPER + 1]  = 0x01; /* request  */
    write_mac(frame + ARP_SHA, g_our_mac);
    write_ip(frame + ARP_SPA, NET_IP_SELF);
    /* THA = zeros (unknown) */
    write_ip(frame + ARP_TPA, target_ip);

    rtl8139_send(frame, ARP_FRAME);
}

void arp_recv(const uint8_t *frame, uint16_t len)
{
    if (len < ARP_FRAME) return;

    uint16_t oper    = ((uint16_t)frame[ARP_OPER] << 8) | frame[ARP_OPER + 1];
    uint32_t spa     = read_ip(frame + ARP_SPA);
    uint32_t tpa     = read_ip(frame + ARP_TPA);
    const uint8_t *sha = frame + ARP_SHA;

    /* Always learn the sender's MAC */
    if (spa != NET_IP_ANY)
        arp_table_set(spa, sha);

    if (oper == 1u && tpa == NET_IP_SELF) {
        /* ARP request directed at us — send a reply */
        uint8_t reply[ARP_FRAME];
        int i;
        for (i = 0; i < ARP_FRAME; i++) reply[i] = 0;

        write_mac(reply + ETH_DST, sha);
        write_mac(reply + ETH_SRC, g_our_mac);
        reply[ETH_TYPE]     = (uint8_t)(ETHERTYPE_ARP >> 8);
        reply[ETH_TYPE + 1] = (uint8_t)(ETHERTYPE_ARP);

        reply[ARP_HTYPE]     = 0x00; reply[ARP_HTYPE + 1] = 0x01;
        reply[ARP_PTYPE]     = 0x08; reply[ARP_PTYPE + 1] = 0x00;
        reply[ARP_HLEN]      = 6;
        reply[ARP_PLEN]      = 4;
        reply[ARP_OPER]      = 0x00; reply[ARP_OPER + 1]  = 0x02; /* reply */
        write_mac(reply + ARP_SHA, g_our_mac);
        write_ip(reply + ARP_SPA, NET_IP_SELF);
        write_mac(reply + ARP_THA, sha);
        write_ip(reply + ARP_TPA, spa);

        rtl8139_send(reply, ARP_FRAME);
    }
}

int arp_resolve(uint32_t ip, uint8_t mac_out[6])
{
    /* Check table first */
    if (arp_table_get(ip, mac_out)) return 1;

    /* Send request and poll for up to ~500 ms (50 ticks @ 100 Hz) */
    arp_send_request(ip);
    uint32_t deadline = pit_get_ticks() + 50u;

    while (pit_get_ticks() < deadline) {
        /* Drain NIC so arp_recv() gets called and fills the table */
        net_poll();
        if (arp_table_get(ip, mac_out)) return 1;
    }
    return 0;
}
