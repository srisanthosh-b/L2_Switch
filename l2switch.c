/*
 * mini_l2_switch - VLAN Aware Linux Layer-2 Ethernet Switch
 *
 * Topology:
 *
 *        PC1 -------- swp0 ---- VLAN 10
 *        PC2 -------- swp1 ---- VLAN 10
 *        PC3 -------- swp2 ---- VLAN 20
 *
 * Features:
 *   - MAC learning
 *   - VLAN-aware MAC learning
 *   - Unknown-unicast flooding
 *   - Broadcast flooding
 *   - Multicast flooding
 *   - VLAN isolation
 *   - /proc MAC table
 *   - /proc statistics
 *   - /proc clear
 *
 * This is a static ACCESS-port VLAN implementation.
 * It does NOT implement 802.1Q trunk/tag handling.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>

#include <linux/hashtable.h>
#include <linux/spinlock.h>

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <linux/netfilter.h>
#include <linux/netfilter_netdev.h>

#include <linux/if_ether.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

/* --------------------------------------------------------- */
/* Configuration                                             */
/* --------------------------------------------------------- */

#define MAC_HASH_BITS 8
#define MAX_PORTS     3

#define VLAN10         10
#define VLAN20         20

/* --------------------------------------------------------- */
/* Module information                                        */
/* --------------------------------------------------------- */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Srisanthosh");
MODULE_DESCRIPTION("Mini VLAN-Aware Layer-2 Ethernet Switch");
MODULE_VERSION("2.0");

/* --------------------------------------------------------- */
/* VLAN configuration                                        */
/* --------------------------------------------------------- */

struct vlan_port {
    unsigned short vlan_id;
};

/*
 * Static access-port VLAN configuration.
 *
 * swp0 -> VLAN 10
 * swp1 -> VLAN 10
 * swp2 -> VLAN 20
 */
static struct vlan_port port_vlan[MAX_PORTS] = {
    [0] = {
        .vlan_id = VLAN10
    },

    [1] = {
        .vlan_id = VLAN10
    },

    [2] = {
        .vlan_id = VLAN20
    }
};

/* --------------------------------------------------------- */
/* MAC address table                                         */
/* --------------------------------------------------------- */

struct mac_entry {
    unsigned char mac[ETH_ALEN];

    /*
     * VLAN is part of the MAC-table entry.
     *
     * Therefore:
     *
     * VLAN 10 + MAC A
     *
     * and
     *
     * VLAN 20 + MAC A
     *
     * are treated as different entries.
     */
    unsigned short vlan_id;

    struct net_device *dev;

    unsigned long last_seen;

    struct hlist_node node;
};

static DEFINE_HASHTABLE(mac_table, MAC_HASH_BITS);

static DEFINE_SPINLOCK(mac_lock);

/* --------------------------------------------------------- */
/* Port statistics                                           */
/* --------------------------------------------------------- */

struct port_stats {
    unsigned long rx_packets;
    unsigned long tx_packets;

    unsigned long forwarded;
    unsigned long flooded;

    unsigned long dropped;

    unsigned long broadcast;
    unsigned long multicast;
    unsigned long unknown_unicast;

    unsigned long vlan_dropped;
};

static struct port_stats stats[MAX_PORTS];

/* --------------------------------------------------------- */
/* Switch port names                                         */
/* --------------------------------------------------------- */

static char port0[IFNAMSIZ] = "swp0";
static char port1[IFNAMSIZ] = "swp1";
static char port2[IFNAMSIZ] = "swp2";

module_param_string(port0, port0, IFNAMSIZ, 0444);
MODULE_PARM_DESC(port0, "Switch port 0");

module_param_string(port1, port1, IFNAMSIZ, 0444);
MODULE_PARM_DESC(port1, "Switch port 1");

module_param_string(port2, port2, IFNAMSIZ, 0444);
MODULE_PARM_DESC(port2, "Switch port 2");

/* --------------------------------------------------------- */
/* Netfilter hooks                                           */
/* --------------------------------------------------------- */

static struct nf_hook_ops nf_ops[MAX_PORTS];

/* --------------------------------------------------------- */
/* Proc filesystem                                           */
/* --------------------------------------------------------- */

static struct proc_dir_entry *proc_dir;

/* --------------------------------------------------------- */
/* Get port name                                             */
/* --------------------------------------------------------- */

static const char *get_port_name(int index)
{
    switch (index) {

    case 0:
        return port0;

    case 1:
        return port1;

    case 2:
        return port2;

    default:
        return NULL;
    }
}

/* --------------------------------------------------------- */
/* Get port index                                            */
/* --------------------------------------------------------- */

static int get_port_index(struct net_device *dev)
{
    int i;

    if (!dev)
        return -1;

    for (i = 0; i < MAX_PORTS; i++) {

        const char *name = get_port_name(i);

        if (name && strcmp(dev->name, name) == 0)
            return i;
    }

    return -1;
}

/* --------------------------------------------------------- */
/* Get VLAN of a port                                        */
/* --------------------------------------------------------- */

static unsigned short get_port_vlan(struct net_device *dev)
{
    int port;

    port = get_port_index(dev);

    if (port < 0)
        return 0;

    return port_vlan[port].vlan_id;
}

/* --------------------------------------------------------- */
/* MAC table lookup                                          */
/* --------------------------------------------------------- */

/*
 * IMPORTANT:
 *
 * lookup_mac() expects mac_lock to already be held.
 *
 * MAC lookup is based on:
 *
 *       VLAN + MAC
 *
 * instead of only:
 *
 *       MAC
 */

static struct mac_entry *
lookup_mac(unsigned short vlan_id, const unsigned char *mac)
{
    struct mac_entry *entry;

    u64 key;

    /*
     * VLAN becomes part of the hash key.
     *
     * Hash collisions are still handled by
     * hash_for_each_possible().
     */
    key = ether_addr_to_u64(mac);

    key ^= ((u64)vlan_id << 48);

    hash_for_each_possible(mac_table, entry, node, key) {

        if (entry->vlan_id != vlan_id)
            continue;

        if (ether_addr_equal(entry->mac, mac))
            return entry;
    }

    return NULL;
}

/* --------------------------------------------------------- */
/* Learn source MAC                                          */
/* --------------------------------------------------------- */

static void learn_mac(struct net_device *dev,
                      unsigned short vlan_id,
                      const unsigned char *mac)
{
    struct mac_entry *entry;

    u64 key;

    /*
     * Never learn multicast/broadcast source addresses.
     */
    if (is_multicast_ether_addr(mac))
        return;

    key = ether_addr_to_u64(mac);

    key ^= ((u64)vlan_id << 48);

    spin_lock_bh(&mac_lock);

    entry = lookup_mac(vlan_id, mac);

    if (entry) {

        /*
         * MAC already known.
         *
         * Update its port and timestamp.
         */
        entry->dev = dev;
        entry->last_seen = jiffies;

        spin_unlock_bh(&mac_lock);

        return;
    }

    /*
     * MAC is new.
     */
    entry = kmalloc(sizeof(*entry), GFP_ATOMIC);

    if (!entry) {

        spin_unlock_bh(&mac_lock);

        pr_err("l2switch: failed to allocate MAC entry\n");

        return;
    }

    ether_addr_copy(entry->mac, mac);

    entry->vlan_id = vlan_id;
    entry->dev = dev;
    entry->last_seen = jiffies;

    hash_add(mac_table, &entry->node, key);

    spin_unlock_bh(&mac_lock);
}

/* --------------------------------------------------------- */
/* Clear MAC table                                           */
/* --------------------------------------------------------- */

static void clear_mac_table(void)
{
    int bucket;

    struct mac_entry *entry;
    struct hlist_node *tmp;

    spin_lock_bh(&mac_lock);

    hash_for_each_safe(mac_table, bucket, tmp, entry, node) {

        hash_del(&entry->node);

        kfree(entry);
    }

    spin_unlock_bh(&mac_lock);

    pr_info("l2switch: MAC table cleared\n");
}

/* --------------------------------------------------------- */
/* Flood packet                                              */
/* --------------------------------------------------------- */

static int flood_packet(struct sk_buff *skb,
                        struct net_device *incoming_dev,
                        unsigned short vlan_id)
{
    struct net_device *ports[MAX_PORTS] = { NULL };

    struct sk_buff *clone;

    int i;

    int flood_count = 0;

    /*
     * Get references to all switch ports.
     */
    for (i = 0; i < MAX_PORTS; i++) {

        ports[i] = dev_get_by_name(&init_net,
                                   get_port_name(i));

        if (!ports[i])
            continue;

        /*
         * VLAN isolation:
         *
         * Do NOT flood to a port belonging
         * to another VLAN.
         */
        if (port_vlan[i].vlan_id != vlan_id) {

            dev_put(ports[i]);

            ports[i] = NULL;

            continue;
        }
    }

    /*
     * Create one copy for every valid
     * output port in the same VLAN.
     */
    for (i = 0; i < MAX_PORTS; i++) {

        if (!ports[i])
            continue;

        /*
         * Never send packet back to ingress port.
         */
        if (ports[i] == incoming_dev) {

            dev_put(ports[i]);

            ports[i] = NULL;

            continue;
        }

        /*
         * Clone packet.
         */
        clone = skb_clone(skb, GFP_ATOMIC);

        if (!clone) {

            stats[i].dropped++;

            dev_put(ports[i]);

            ports[i] = NULL;

            continue;
        }

        /*
         * At this point, skb->data may not point to
         * the Ethernet header.
         *
         * Restore Ethernet header space.
         */
        if (skb_headroom(clone) < ETH_HLEN) {

            kfree_skb(clone);

            stats[i].dropped++;

            dev_put(ports[i]);

            ports[i] = NULL;

            continue;
        }

        skb_push(clone, ETH_HLEN);

        /*
         * Tell Linux where this packet should
         * be transmitted.
         */
        clone->dev = ports[i];

        /*
         * Reset MAC header.
         */
        skb_reset_mac_header(clone);

        /*
         * Enter Linux transmit path.
         */
        if (dev_queue_xmit(clone) == NET_XMIT_SUCCESS) {

            stats[i].tx_packets++;

            flood_count++;
        }

        dev_put(ports[i]);

        ports[i] = NULL;
    }

    /*
     * Flooded means number of output copies.
     */
    if (flood_count > 0) {

        int in_port = get_port_index(incoming_dev);

        if (in_port >= 0)
            stats[in_port].flooded += flood_count;
    }

    return flood_count;
}

/* --------------------------------------------------------- */
/* Forward known unicast                                    */
/* --------------------------------------------------------- */

static int forward_packet(struct sk_buff *skb,
                          struct net_device *out_dev,
                          unsigned short vlan_id)
{
    struct sk_buff *clone;

    int out_port;

    /*
     * Make sure destination port belongs
     * to the same VLAN.
     */
    if (get_port_vlan(out_dev) != vlan_id) {

        out_port = get_port_index(skb->dev);

        if (out_port >= 0)
            stats[out_port].vlan_dropped++;

        return -EPERM;
    }

    clone = skb_clone(skb, GFP_ATOMIC);

    if (!clone)
        return -ENOMEM;

    /*
     * Make Ethernet header available.
     */
    if (skb_headroom(clone) < ETH_HLEN) {

        kfree_skb(clone);

        return -ENOMEM;
    }

    skb_push(clone, ETH_HLEN);

    /*
     * Set output device.
     */
    clone->dev = out_dev;

    /*
     * Reset MAC header.
     */
    skb_reset_mac_header(clone);

    /*
     * Transmit.
     */
    dev_queue_xmit(clone);

    out_port = get_port_index(out_dev);

    if (out_port >= 0)
        stats[out_port].tx_packets++;

    return 0;
}

/* --------------------------------------------------------- */
/* Netfilter hook                                            */
/* --------------------------------------------------------- */

static unsigned int
l2switch_hook(void *priv,
              struct sk_buff *skb,
              const struct nf_hook_state *state)
{
    struct net_device *in_dev;

    struct ethhdr *eth;

    struct mac_entry *entry;

    unsigned short vlan_id;

    int in_port;

    int out_port;

    struct net_device *out_dev;

    if (!skb)
        return NF_ACCEPT;

    in_dev = state->in;

    if (!in_dev)
        return NF_ACCEPT;

    /*
     * Make sure packet arrived through one
     * of our switch ports.
     */
    in_port = get_port_index(in_dev);

    if (in_port < 0)
        return NF_ACCEPT;

    /*
     * Get VLAN assigned to ingress port.
     */
    vlan_id = port_vlan[in_port].vlan_id;

    /*
     * Make sure complete Ethernet header exists.
     */
    if (!pskb_may_pull(skb, ETH_HLEN)) {

        stats[in_port].dropped++;

        return NF_DROP;
    }

    eth = eth_hdr(skb);

    stats[in_port].rx_packets++;

    /*
     * --------------------------------------------------
     * 1. LEARN SOURCE MAC
     * --------------------------------------------------
     *
     * Learning is VLAN-aware.
     *
     * VLAN 10 + MAC A
     *
     * is different from
     *
     * VLAN 20 + MAC A
     */
    learn_mac(in_dev, vlan_id, eth->h_source);

    /*
     * --------------------------------------------------
     * 2. BROADCAST
     * --------------------------------------------------
     */

    if (is_broadcast_ether_addr(eth->h_dest)) {

        stats[in_port].broadcast++;

        flood_packet(skb, in_dev, vlan_id);

        /*
         * Original packet must not continue
         * through the normal Linux receive path.
         */
        return NF_DROP;
    }

    /*
     * --------------------------------------------------
     * 3. MULTICAST
     * --------------------------------------------------
     */

    if (is_multicast_ether_addr(eth->h_dest)) {

        stats[in_port].multicast++;

        flood_packet(skb, in_dev, vlan_id);

        return NF_DROP;
    }

    /*
     * --------------------------------------------------
     * 4. LOOKUP DESTINATION MAC
     * --------------------------------------------------
     */

    spin_lock_bh(&mac_lock);

    entry = lookup_mac(vlan_id, eth->h_dest);

    if (entry) {

        out_dev = entry->dev;

        out_port = get_port_index(out_dev);

        /*
         * Destination is on same ingress port.
         */
        if (out_dev == in_dev) {

            spin_unlock_bh(&mac_lock);

            /*
             * Drop instead of sending packet back
             * to same port.
             */
            stats[in_port].dropped++;

            return NF_DROP;
        }

        /*
         * Verify VLAN again.
         */
        if (out_port < 0 ||
            port_vlan[out_port].vlan_id != vlan_id) {

            spin_unlock_bh(&mac_lock);

            stats[in_port].vlan_dropped++;

            return NF_DROP;
        }

        /*
         * Hold a reference to output device
         * before releasing MAC-table lock.
         */
        dev_hold(out_dev);

        spin_unlock_bh(&mac_lock);

        /*
         * Forward packet.
         */
        if (forward_packet(skb, out_dev, vlan_id) == 0)
            stats[in_port].forwarded++;
        else
            stats[in_port].dropped++;

        dev_put(out_dev);

        /*
         * Original skb is consumed by our switching
         * logic, so don't let it continue.
         */
        return NF_DROP;
    }

    spin_unlock_bh(&mac_lock);

    /*
     * --------------------------------------------------
     * 5. UNKNOWN UNICAST
     * --------------------------------------------------
     */

    stats[in_port].unknown_unicast++;

    flood_packet(skb, in_dev, vlan_id);

    return NF_DROP;
}

/* --------------------------------------------------------- */
/* /proc MAC table                                           */
/* --------------------------------------------------------- */

static int mac_proc_show(struct seq_file *m, void *v)
{
    int bucket;

    struct mac_entry *entry;

    seq_puts(m,
             "MAC Address       VLAN    Port       Age\n");

    seq_puts(m,
             "--------------------------------------------\n");

    spin_lock_bh(&mac_lock);

    hash_for_each(mac_table, bucket, entry, node) {

        seq_printf(m,
                   "%pM    %4u    %-8s   %lu sec\n",
                   entry->mac,
                   entry->vlan_id,
                   entry->dev ? entry->dev->name : "unknown",
                   (jiffies - entry->last_seen) / HZ);
    }

    spin_unlock_bh(&mac_lock);

    return 0;
}

static int mac_proc_open(struct inode *inode,
                         struct file *file)
{
    return single_open(file,
                       mac_proc_show,
                       NULL);
}

static const struct proc_ops mac_proc_ops = {
    .proc_open    = mac_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* --------------------------------------------------------- */
/* /proc statistics                                          */
/* --------------------------------------------------------- */

static int stats_proc_show(struct seq_file *m, void *v)
{
    int i;

    for (i = 0; i < MAX_PORTS; i++) {

        seq_printf(m,
                   "\nPort: %s\n",
                   get_port_name(i));

        seq_printf(m,
                   "VLAN: %u\n",
                   port_vlan[i].vlan_id);

        seq_printf(m,
                   "RX packets      : %lu\n",
                   stats[i].rx_packets);

        seq_printf(m,
                   "TX packets      : %lu\n",
                   stats[i].tx_packets);

        seq_printf(m,
                   "Forwarded       : %lu\n",
                   stats[i].forwarded);

        seq_printf(m,
                   "Flooded         : %lu\n",
                   stats[i].flooded);

        seq_printf(m,
                   "Dropped         : %lu\n",
                   stats[i].dropped);

        seq_printf(m,
                   "Broadcast       : %lu\n",
                   stats[i].broadcast);

        seq_printf(m,
                   "Multicast       : %lu\n",
                   stats[i].multicast);

        seq_printf(m,
                   "Unknown unicast : %lu\n",
                   stats[i].unknown_unicast);

        seq_printf(m,
                   "VLAN dropped    : %lu\n",
                   stats[i].vlan_dropped);
    }

    return 0;
}

static int stats_proc_open(struct inode *inode,
                           struct file *file)
{
    return single_open(file,
                       stats_proc_show,
                       NULL);
}

static const struct proc_ops stats_proc_ops = {
    .proc_open    = stats_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* --------------------------------------------------------- */
/* /proc clear                                               */
/* --------------------------------------------------------- */

static ssize_t clear_proc_write(struct file *file,
                                const char __user *buffer,
                                size_t count,
                                loff_t *ppos)
{
    /*
     * We don't actually need to inspect the data.
     *
     * Any write to /proc/l2switch/clear
     * clears the MAC table.
     */
    clear_mac_table();

    return count;
}

static const struct proc_ops clear_proc_ops = {
    .proc_write = clear_proc_write,
};

/* --------------------------------------------------------- */
/* Module initialization                                     */
/* --------------------------------------------------------- */

static int __init l2switch_init(void)
{
    int i;

    int ret;

    struct net_device *dev;

    pr_info("l2switch: loading VLAN-aware L2 switch\n");

    /*
     * Initialize hash table.
     */
    hash_init(mac_table);

    /*
     * Clear statistics.
     */
    memset(stats, 0, sizeof(stats));

    /*
     * Register one Netfilter ingress hook
     * for every switch port.
     */
    for (i = 0; i < MAX_PORTS; i++) {

        dev = dev_get_by_name(&init_net,
                              get_port_name(i));

        if (!dev) {

            pr_err("l2switch: device %s not found\n",
                   get_port_name(i));

            ret = -ENODEV;

            goto error_hooks;
        }

        /*
         * Configure hook.
         */
        nf_ops[i].hook = l2switch_hook;

        nf_ops[i].dev = dev;

        nf_ops[i].pf = NFPROTO_NETDEV;

        nf_ops[i].hooknum = NF_NETDEV_INGRESS;

        nf_ops[i].priority = 0;

        ret = nf_register_net_hook(&init_net,
                                   &nf_ops[i]);

        dev_put(dev);

        if (ret < 0) {

            pr_err("l2switch: failed to register hook for %s\n",
                   get_port_name(i));

            goto error_hooks;
        }

        pr_info("l2switch: %s -> VLAN %u\n",
                get_port_name(i),
                port_vlan[i].vlan_id);
    }

    /*
     * Create /proc/l2switch.
     */
    proc_dir = proc_mkdir("l2switch", NULL);

    if (!proc_dir) {

        pr_err("l2switch: failed to create /proc/l2switch\n");

        ret = -ENOMEM;

        goto error_hooks;
    }

    /*
     * MAC table.
     */
    if (!proc_create("mac",
                     0444,
                     proc_dir,
                     &mac_proc_ops)) {

        pr_err("l2switch: failed to create /proc/l2switch/mac\n");

        ret = -ENOMEM;

        goto error_proc;
    }

    /*
     * Statistics.
     */
    if (!proc_create("stats",
                     0444,
                     proc_dir,
                     &stats_proc_ops)) {

        pr_err("l2switch: failed to create /proc/l2switch/stats\n");

        ret = -ENOMEM;

        goto error_proc;
    }

    /*
     * MAC table clear interface.
     */
    if (!proc_create("clear",
                     0200,
                     proc_dir,
                     &clear_proc_ops)) {

        pr_err("l2switch: failed to create /proc/l2switch/clear\n");

        ret = -ENOMEM;

        goto error_proc;
    }

    pr_info("l2switch: module loaded successfully\n");

    return 0;

/* --------------------------------------------------------- */
/* Error handling                                            */
/* --------------------------------------------------------- */

error_proc:

    remove_proc_subtree("l2switch", NULL);

    proc_dir = NULL;

error_hooks:

    /*
     * Unregister hooks that were successfully registered.
     */
    while (--i >= 0)
        nf_unregister_net_hook(&init_net,
                               &nf_ops[i]);

    clear_mac_table();

    return ret;
}

/* --------------------------------------------------------- */
/* Module cleanup                                            */
/* --------------------------------------------------------- */

static void __exit l2switch_exit(void)
{
    int i;

    pr_info("l2switch: unloading module\n");

    /*
     * Remove Netfilter hooks.
     */
    for (i = 0; i < MAX_PORTS; i++)
        nf_unregister_net_hook(&init_net,
                               &nf_ops[i]);

    /*
     * Remove proc entries.
     */
    remove_proc_subtree("l2switch", NULL);

    proc_dir = NULL;

    /*
     * Free MAC table.
     */
    clear_mac_table();

    pr_info("l2switch: module unloaded\n");
}

module_init(l2switch_init);
module_exit(l2switch_exit);
