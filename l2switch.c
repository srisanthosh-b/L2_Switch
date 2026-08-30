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

#define MAC_HASH_BITS 8
#define MAX_PORTS 3

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sri Santhosh");
MODULE_DESCRIPTION("Mini Linux Layer-2 Ethernet Switch");
MODULE_VERSION("1.0");


/*
 * ============================================================
 * MAC TABLE
 * ============================================================
 */

struct mac_entry {
    unsigned char mac[ETH_ALEN];

    struct net_device *dev;

    unsigned long last_seen;

    struct hlist_node node;
};

static DEFINE_HASHTABLE(mac_table, MAC_HASH_BITS);

static DEFINE_SPINLOCK(mac_lock);


/*
 * ============================================================
 * PORT STATISTICS
 * ============================================================
 */

struct port_stats {

    unsigned long rx_packets;

    unsigned long tx_packets;

    unsigned long forwarded;

    unsigned long flooded;

    unsigned long dropped;

    unsigned long broadcast;

    unsigned long multicast;

    unsigned long unknown_unicast;
};

static struct port_stats stats[MAX_PORTS];


/*
 * ============================================================
 * SWITCH PORT NAMES
 * ============================================================
 *
 * These interfaces are created by setup.sh.
 *
 * swp0 <-> pc1
 * swp1 <-> pc2
 * swp2 <-> pc3
 *
 * ============================================================
 */

static char port0[IFNAMSIZ] = "swp0";
static char port1[IFNAMSIZ] = "swp1";
static char port2[IFNAMSIZ] = "swp2";

module_param_string(port0, port0, IFNAMSIZ, 0644);
MODULE_PARM_DESC(port0, "Switch port 0");

module_param_string(port1, port1, IFNAMSIZ, 0644);
MODULE_PARM_DESC(port1, "Switch port 1");

module_param_string(port2, port2, IFNAMSIZ, 0644);
MODULE_PARM_DESC(port2, "Switch port 2");


/*
 * ============================================================
 * PROC DIRECTORY
 * ============================================================
 */

static struct proc_dir_entry *proc_dir;


/*
 * ============================================================
 * NETFILTER HOOKS
 * ============================================================
 *
 * One hook is registered for each switch port.
 *
 * ============================================================
 */

static struct nf_hook_ops nf_ops[MAX_PORTS];


/*
 * ============================================================
 * GET PORT INDEX
 * ============================================================
 */

static int get_port_index(struct net_device *dev)
{
    if (!dev)
        return -1;

    if (strcmp(dev->name, port0) == 0)
        return 0;

    if (strcmp(dev->name, port1) == 0)
        return 1;

    if (strcmp(dev->name, port2) == 0)
        return 2;

    return -1;
}


/*
 * ============================================================
 * MAC TABLE LOOKUP
 * ============================================================
 *
 * Caller must hold mac_lock.
 *
 * ============================================================
 */

static struct mac_entry *lookup_mac(const unsigned char *mac)
{
    struct mac_entry *entry;

    hash_for_each_possible(mac_table,
                           entry,
                           node,
                           ether_addr_to_u64(mac)) {

        if (ether_addr_equal(entry->mac, mac))
            return entry;
    }

    return NULL;
}


/*
 * ============================================================
 * LEARN SOURCE MAC
 * ============================================================
 */

static void learn_mac(struct net_device *dev,
                      const unsigned char *mac)
{
    struct mac_entry *entry;

    /*
     * Never learn multicast/broadcast source addresses.
     */
    if (is_multicast_ether_addr(mac))
        return;

    spin_lock_bh(&mac_lock);

    entry = lookup_mac(mac);

    /*
     * MAC already exists.
     */
    if (entry) {

        /*
         * MAC may have moved to another port.
         */
        entry->dev = dev;

        entry->last_seen = jiffies;

        spin_unlock_bh(&mac_lock);

        return;
    }


    /*
     * Create a new MAC table entry.
     */
    entry = kmalloc(sizeof(*entry), GFP_ATOMIC);

    if (!entry) {

        spin_unlock_bh(&mac_lock);

        return;
    }


    ether_addr_copy(entry->mac, mac);

    entry->dev = dev;

    entry->last_seen = jiffies;


    hash_add(mac_table,
             &entry->node,
             ether_addr_to_u64(mac));


    spin_unlock_bh(&mac_lock);


    pr_info("l2switch: learned %pM -> %s\n",
            mac,
            dev->name);
}


/*
 * ============================================================
 * CLEAR MAC TABLE
 * ============================================================
 */

static void clear_mac_table(void)
{
    struct mac_entry *entry;

    struct hlist_node *tmp;

    int bucket;


    spin_lock_bh(&mac_lock);


    hash_for_each_safe(mac_table,
                       bucket,
                       tmp,
                       entry,
                       node) {

        hash_del(&entry->node);

        kfree(entry);
    }


    spin_unlock_bh(&mac_lock);
}


/*
 * ============================================================
 * FLOOD PACKET
 * ============================================================
 *
 * Flood packet to every switch port except the incoming port.
 *
 * IMPORTANT:
 *
 * At NF_NETDEV_INGRESS, skb->data is not necessarily pointing
 * to the beginning of the Ethernet header.
 *
 * Before dev_queue_xmit(), we restore ETH_HLEN bytes using
 * skb_push().
 *
 * ============================================================
 */

static void flood_packet(struct sk_buff *skb,
                         struct net_device *incoming_dev)
{
    struct net_device *ports[MAX_PORTS];

    struct sk_buff *clone;

    int i;

    int count = 0;


    ports[0] = dev_get_by_name(&init_net, port0);
    ports[1] = dev_get_by_name(&init_net, port1);
    ports[2] = dev_get_by_name(&init_net, port2);


    for (i = 0; i < MAX_PORTS; i++) {

        if (!ports[i])
            continue;


        /*
         * Never send back to incoming port.
         */
        if (ports[i] == incoming_dev) {

            dev_put(ports[i]);

            continue;
        }


        /*
         * Clone the packet.
         */
        clone = skb_clone(skb, GFP_ATOMIC);

        if (!clone) {

            pr_err("l2switch: skb_clone failed for %s\n",
                   ports[i]->name);

            dev_put(ports[i]);

            continue;
        }


        /*
         * We need space for Ethernet header.
         */
        if (skb_headroom(clone) < ETH_HLEN) {

            pr_err("l2switch: insufficient skb headroom\n");

            kfree_skb(clone);

            dev_put(ports[i]);

            continue;
        }


        /*
         * Restore Ethernet header to skb->data.
         */
        skb_push(clone, ETH_HLEN);


        /*
         * Set output device.
         */
        clone->dev = ports[i];


        /*
         * Reset MAC header.
         */
        skb_reset_mac_header(clone);


        pr_info("l2switch: flooding %s -> %s\n",
                incoming_dev->name,
                ports[i]->name);


        /*
         * Transmit packet.
         */
        dev_queue_xmit(clone);


        /*
         * Update output statistics.
         */
        stats[i].tx_packets++;


        count++;


        dev_put(ports[i]);
    }


    /*
     * Count number of flood copies generated
     * by the incoming port.
     */
    {
        int port = get_port_index(incoming_dev);

        if (port >= 0)
            stats[port].flooded += count;
    }
}


/*
 * ============================================================
 * FORWARD PACKET
 * ============================================================
 *
 * Used when destination MAC is known.
 *
 * ============================================================
 */

static void forward_packet(struct sk_buff *skb,
                           struct net_device *out_dev)
{
    struct sk_buff *clone;

    int in_port;

    int out_port;


    in_port = get_port_index(skb->dev);

    out_port = get_port_index(out_dev);


    /*
     * Clone packet.
     */
    clone = skb_clone(skb, GFP_ATOMIC);

    if (!clone) {

        if (in_port >= 0)
            stats[in_port].dropped++;

        return;
    }


    /*
     * Ensure enough headroom exists for Ethernet header.
     */
    if (skb_headroom(clone) < ETH_HLEN) {

        pr_err("l2switch: insufficient skb headroom\n");

        kfree_skb(clone);

        if (in_port >= 0)
            stats[in_port].dropped++;

        return;
    }


    /*
     * Restore Ethernet header.
     */
    skb_push(clone, ETH_HLEN);


    /*
     * Set output interface.
     */
    clone->dev = out_dev;


    /*
     * Reset MAC header.
     */
    skb_reset_mac_header(clone);


    pr_info("l2switch: forwarding %s -> %s\n",
            skb->dev->name,
            out_dev->name);


    /*
     * Transmit.
     */
    dev_queue_xmit(clone);


    /*
     * Statistics.
     */
    if (in_port >= 0)
        stats[in_port].forwarded++;

    if (out_port >= 0)
        stats[out_port].tx_packets++;
}


/*
 * ============================================================
 * PACKET PROCESSING
 * ============================================================
 */

static unsigned int l2switch_hook(void *priv,
                                  struct sk_buff *skb,
                                  const struct nf_hook_state *state)
{
    struct ethhdr *eth;

    struct mac_entry *dst_entry;

    struct net_device *in_dev;

    int port;


    if (!skb)
        return NF_ACCEPT;


    in_dev = skb->dev;


    /*
     * Determine switch port.
     */
    port = get_port_index(in_dev);


    /*
     * Packet did not arrive on one of our switch ports.
     */
    if (port < 0)
        return NF_ACCEPT;


    /*
     * Make sure Ethernet header is accessible.
     */
    if (!pskb_may_pull(skb, ETH_HLEN))
        return NF_ACCEPT;


    /*
     * Get Ethernet header.
     */
    eth = eth_hdr(skb);

    if (!eth)
        return NF_ACCEPT;


    /*
     * RX packet.
     */
    stats[port].rx_packets++;


    /*
     * Learn source MAC.
     */
    if (!is_multicast_ether_addr(eth->h_source))
        learn_mac(in_dev, eth->h_source);


    /*
     * ========================================================
     * BROADCAST
     * ========================================================
     */

    if (is_broadcast_ether_addr(eth->h_dest)) {

        stats[port].broadcast++;

        flood_packet(skb, in_dev);

        return NF_DROP;
    }


    /*
     * ========================================================
     * MULTICAST
     * ========================================================
     */

    if (is_multicast_ether_addr(eth->h_dest)) {

        stats[port].multicast++;

        flood_packet(skb, in_dev);

        return NF_DROP;
    }


    /*
     * ========================================================
     * DESTINATION MAC LOOKUP
     * ========================================================
     */

    spin_lock_bh(&mac_lock);


    dst_entry = lookup_mac(eth->h_dest);


    if (dst_entry)
        dst_entry->last_seen = jiffies;


    /*
     * Known destination.
     */
    if (dst_entry &&
        dst_entry->dev &&
        dst_entry->dev != in_dev) {

        struct net_device *out_dev;

        out_dev = dst_entry->dev;

        dev_hold(out_dev);


        spin_unlock_bh(&mac_lock);


        forward_packet(skb, out_dev);


        dev_put(out_dev);


        return NF_DROP;
    }


    spin_unlock_bh(&mac_lock);


    /*
     * ========================================================
     * UNKNOWN UNICAST
     * ========================================================
     */

    stats[port].unknown_unicast++;


    flood_packet(skb, in_dev);


    return NF_DROP;
}


/*
 * ============================================================
 * /PROC MAC TABLE
 * ============================================================
 */

static int mac_proc_show(struct seq_file *m,
                         void *v)
{
    struct mac_entry *entry;

    unsigned long age;

    int bucket;


    seq_puts(m,
             "MAC Address         Port       Age(sec)\n");

    seq_puts(m,
             "----------------------------------------\n");


    spin_lock_bh(&mac_lock);


    hash_for_each(mac_table,
                  bucket,
                  entry,
                  node) {

        age = (jiffies - entry->last_seen) / HZ;


        seq_printf(m,
                   "%pM        %-8s %lu\n",
                   entry->mac,
                   entry->dev->name,
                   age);
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

    .proc_open = mac_proc_open,

    .proc_read = seq_read,

    .proc_lseek = seq_lseek,

    .proc_release = single_release,
};


/*
 * ============================================================
 * /PROC STATISTICS
 * ============================================================
 */

static int stats_proc_show(struct seq_file *m,
                           void *v)
{
    int i;


    seq_puts(m,
             "L2 SWITCH STATISTICS\n");

    seq_puts(m,
             "===================\n\n");


    for (i = 0; i < MAX_PORTS; i++) {

        const char *name;


        if (i == 0)
            name = port0;
        else if (i == 1)
            name = port1;
        else
            name = port2;


        seq_printf(m,
                   "PORT: %s\n",
                   name);


        seq_printf(m,
                   "RX packets       : %lu\n",
                   stats[i].rx_packets);


        seq_printf(m,
                   "TX packets       : %lu\n",
                   stats[i].tx_packets);


        seq_printf(m,
                   "Forwarded        : %lu\n",
                   stats[i].forwarded);


        seq_printf(m,
                   "Flooded          : %lu\n",
                   stats[i].flooded);


        seq_printf(m,
                   "Dropped          : %lu\n",
                   stats[i].dropped);


        seq_printf(m,
                   "Broadcast        : %lu\n",
                   stats[i].broadcast);


        seq_printf(m,
                   "Multicast        : %lu\n",
                   stats[i].multicast);


        seq_printf(m,
                   "Unknown Unicast  : %lu\n",
                   stats[i].unknown_unicast);


        seq_puts(m, "\n");
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

    .proc_open = stats_proc_open,

    .proc_read = seq_read,

    .proc_lseek = seq_lseek,

    .proc_release = single_release,
};


/*
 * ============================================================
 * MODULE INITIALIZATION
 * ============================================================
 */

static int __init l2switch_init(void)
{
    int ret;

    int i;

    struct net_device *dev;

    struct proc_dir_entry *mac_file;

    struct proc_dir_entry *stats_file;


    const char *port_names[MAX_PORTS] = {
        port0,
        port1,
        port2
    };


    pr_info("l2switch: loading module\n");


    /*
     * Initialize MAC hash table.
     */
    hash_init(mac_table);


    /*
     * --------------------------------------------------------
     * Register one NF_NETDEV ingress hook per switch port.
     * --------------------------------------------------------
     */

    for (i = 0; i < MAX_PORTS; i++) {

        dev = dev_get_by_name(&init_net,
                              port_names[i]);


        if (!dev) {

            pr_err("l2switch: device %s not found\n",
                   port_names[i]);


            while (--i >= 0)
                nf_unregister_net_hook(&init_net,
                                       &nf_ops[i]);


            return -ENODEV;
        }


        nf_ops[i].hook = l2switch_hook;

        nf_ops[i].dev = dev;

        nf_ops[i].pf = NFPROTO_NETDEV;

        nf_ops[i].hooknum = NF_NETDEV_INGRESS;

        nf_ops[i].priority = 0;


        ret = nf_register_net_hook(&init_net,
                                   &nf_ops[i]);


        dev_put(dev);


        if (ret) {

            pr_err("l2switch: failed to register hook "
                   "on %s: error=%d\n",
                   port_names[i],
                   ret);


            while (--i >= 0)
                nf_unregister_net_hook(&init_net,
                                       &nf_ops[i]);


            return ret;
        }


        pr_info("l2switch: hook registered on %s\n",
                port_names[i]);
    }


    /*
     * --------------------------------------------------------
     * Create /proc/l2switch
     * --------------------------------------------------------
     */

    proc_dir = proc_mkdir("l2switch", NULL);


    if (!proc_dir) {

        pr_err("l2switch: failed to create /proc/l2switch\n");


        for (i = 0; i < MAX_PORTS; i++)
            nf_unregister_net_hook(&init_net,
                                   &nf_ops[i]);


        return -ENOMEM;
    }


    /*
     * Create MAC table file.
     */
    mac_file = proc_create("mac",
                           0444,
                           proc_dir,
                           &mac_proc_ops);


    if (!mac_file) {

        pr_err("l2switch: failed to create /proc/l2switch/mac\n");

        proc_remove(proc_dir);

        for (i = 0; i < MAX_PORTS; i++)
            nf_unregister_net_hook(&init_net,
                                   &nf_ops[i]);

        return -ENOMEM;
    }


    /*
     * Create statistics file.
     */
    stats_file = proc_create("stats",
                             0444,
                             proc_dir,
                             &stats_proc_ops);


    if (!stats_file) {

        pr_err("l2switch: failed to create /proc/l2switch/stats\n");

        proc_remove(proc_dir);

        for (i = 0; i < MAX_PORTS; i++)
            nf_unregister_net_hook(&init_net,
                                   &nf_ops[i]);

        return -ENOMEM;
    }


    pr_info("l2switch: loaded successfully\n");

    pr_info("l2switch: ports = %s %s %s\n",
            port0,
            port1,
            port2);


    return 0;
}


/*
 * ============================================================
 * MODULE CLEANUP
 * ============================================================
 */

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
     * Remove /proc/l2switch and everything below it.
     */
    if (proc_dir)
        proc_remove(proc_dir);


    /*
     * Clear MAC table.
     */
    clear_mac_table();


    pr_info("l2switch: unloaded\n");
}


module_init(l2switch_init);

module_exit(l2switch_exit);
