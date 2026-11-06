// SPDX-License-Identifier: GPL-2.0
/*
 * apcnet - the fast path to Aleph One !PC's NE2000.
 *
 * !PC's NE2000 is software on the ARM, and every register access and every
 * word of a frame through its data port is a trip through the Gemini
 * mailbox. !PC 3.09 (pcpro3, branch apcnet) adds a fast path beside it on
 * ports 0x310-0x317: the frames go through two rings in the PC's own
 * memory, which !PC reads and writes directly, so sending costs the PC one
 * port write and receiving one port read per batch. It is the same card -
 * the same PCNE2 connection (PCNAT, or a real network), Ethernet address
 * and IRQ - so when the fast path is there this driver takes the NE2000's
 * ports too, and ne.c, probing later, leaves the card alone. On a !PC
 * without it nothing is claimed and ne.c drives the NE2000 as before.
 *
 * The protocol is described in !PC's DEV.c.NE2000, under "Fast path"; the
 * definitions here must match it. In short:
 *
 *   0x310  write: command; read: 0x4E50 if the fast path is there
 *   0x312  write: parameter bits 0-15; read: version (1)
 *   0x314  write: parameter bits 16-31; read: how the last attach went
 *   0x316  write: tx_head's low 16 bits, after queueing frames, unless
 *          !PC's tx_busy says it is sending already;
 *          read: what interrupted (APN_INT_*), which also clears it
 *
 * The block holding both rings is physically contiguous, and uncached while
 * !PC has it, as for apcdisk's shared buffer: on a PC card the PC's cache
 * would otherwise keep copies of what the ARM writes behind it, and hold
 * back the PC's own writes from the ARM. The Gemini card's write FIFO can
 * still deliver memory writes after a later port write, so !PC waits, on a
 * callback once the PC has gone on, for the values the port writes name
 * (tx_head, the magic word) to arrive; attaching is answered in the
 * status word a little after the port write.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <asm/set_memory.h>
#include <asm/special_insns.h>

#define APN_NAME	"apcnet"

static int io = 0x310;
module_param(io, int, 0444);
MODULE_PARM_DESC(io, "fast path ports (default 0x310)");
/* The NE2000's IRQ, !PC's NE2000Int. */
static int irq = 5;
module_param(irq, int, 0444);
MODULE_PARM_DESC(irq, "IRQ (default 5, !PC's NE2000Int)");
static bool disable;
module_param(disable, bool, 0444);
MODULE_PARM_DESC(disable, "leave !PC's NE2000 to ne.c");

#define APN_PORTS	8
#define APN_CMD		0
#define APN_PARLO	2
#define APN_PARHI	4
#define APN_KICK	6

/* The NE2000 the fast path belongs to, kept from ne.c. */
#define NE_IO		0x340
#define NE_PORTS	0x20

#define APN_ID		0x4E50
#define APN_VERSION	1

#define APN_CMD_ATTACH	1
#define APN_CMD_DETACH	2
#define APN_CMD_RXOPTS	3
#define APN_CMD_RXKICK	4

/* What reading APN_PARHI says about the last attach: */
#define APN_ATTACH_NONE		0	/* never asked */
#define APN_ATTACH_PENDING	2	/* asked, not yet carried out */
#define APN_ATTACH_ADDRESS	(-3)	/* not memory !PC can reach */
/* or 1, attached, or -1 (no magic word) or -2 (bad layout), as W_STATUS */

#define APN_INT_RX	1
#define APN_INT_TXSPACE	2

#define APN_F_TXSPACE	1

#define RCR_BROADCAST	0x04
#define RCR_MULTICAST	0x08
#define RCR_PROMISC	0x10

#define APN_MAGIC	0x4E435041	/* "APCN" */

/* Word offsets in the shared block, as in !PC. */
enum {
	W_MAGIC, W_VERSION, W_SIZE, W_TXOFF, W_TXSIZE, W_RXOFF, W_RXSIZE,
	W_TXHEAD, W_TXTAIL, W_RXHEAD, W_RXTAIL, W_FLAGS, W_RXSTALLED,
	W_STATUS, W_MAC0, W_MAC1, W_MCAST0, W_MCAST1,
	W_TXFRAMES, W_TXERRORS, W_RXFRAMES, W_RXDROPPED, W_TXBUSY,
	W_HDRWORDS = 32
};

/* 64K: the header's page, a 16K send ring and a 32K receive ring. */
#define APN_ORDER	4
#define APN_PAGES	(1 << APN_ORDER)
#define APN_SIZE	(PAGE_SIZE << APN_ORDER)
#define APN_TXOFF	0x1000
#define APN_TXSIZE	0x4000
#define APN_RXOFF	0x8000
#define APN_RXSIZE	0x8000

#define APN_MAXFRAME	1514
#define APN_MAXREC	((4 + APN_MAXFRAME + 3) & ~3)

struct apn_priv {
	u32 *sh;			/* the shared block, uncached */
	u8 *txring, *rxring;
	unsigned long virt;
	phys_addr_t phys;
	spinlock_t lock;		/* the send ring and flags */
	bool attached;
};

static struct net_device *apn_dev;

static inline u32 sh_read(struct apn_priv *p, int w)
{
	return READ_ONCE(p->sh[w]);
}

static inline void sh_write(struct apn_priv *p, int w, u32 v)
{
	WRITE_ONCE(p->sh[w], v);
}

/* ---- Rings ---- */

static void ring_write(u8 *ring, u32 size, u32 pos, const void *src, u32 len)
{
	u32 off = pos & (size - 1), first = min(size - off, len);

	memcpy(ring + off, src, first);
	if (len > first)
		memcpy(ring, (const u8 *)src + first, len - first);
}

static void ring_read(void *dst, const u8 *ring, u32 size, u32 pos, u32 len)
{
	u32 off = pos & (size - 1), first = min(size - off, len);

	memcpy(dst, ring + off, first);
	if (len > first)
		memcpy((u8 *)dst + first, ring, len - first);
}

static inline u32 ring_word(const u8 *ring, u32 size, u32 pos)
{
	return READ_ONCE(*(const u32 *)(ring + (pos & (size - 1))));
}

/* ---- Attaching ---- */

static int apn_attach(struct net_device *dev)
{
	struct apn_priv *p = netdev_priv(dev);
	unsigned long start, end;
	int status;

	sh_write(p, W_MAGIC, 0);
	sh_write(p, W_VERSION, APN_VERSION);
	sh_write(p, W_SIZE, APN_SIZE);
	sh_write(p, W_TXOFF, APN_TXOFF);
	sh_write(p, W_TXSIZE, APN_TXSIZE);
	sh_write(p, W_RXOFF, APN_RXOFF);
	sh_write(p, W_RXSIZE, APN_RXSIZE);
	sh_write(p, W_TXHEAD, 0);
	sh_write(p, W_TXTAIL, 0);
	sh_write(p, W_RXHEAD, 0);
	sh_write(p, W_RXTAIL, 0);
	sh_write(p, W_FLAGS, 0);
	sh_write(p, W_RXSTALLED, 0);
	sh_write(p, W_STATUS, 0);
	sh_write(p, W_MCAST0, 0);
	sh_write(p, W_MCAST1, 0);
	wmb();
	/* Last: !PC waits for it, so the rest is there when it looks. */
	sh_write(p, W_MAGIC, APN_MAGIC);
	wmb();

	outw(p->phys & 0xFFFF, dev->base_addr + APN_PARLO);
	outw(p->phys >> 16, dev->base_addr + APN_PARHI);
	outw(APN_CMD_ATTACH, dev->base_addr + APN_CMD);

	/* !PC looks at the block on a callback, once the writes above have
	   left the Gemini card's write FIFO, which they can't while the OUT
	   holds the PC in the mailbox; so wait for its answer. */
	start = jiffies;
	end = start + 2 * HZ;
	while (sh_read(p, W_STATUS) == 0 && time_before(jiffies, end))
		msleep(1);
	status = sh_read(p, W_STATUS);
	if (status == 0) {
		/* What !PC says through the mailbox, which nothing caches. */
		s16 port = inw(dev->base_addr + APN_PARHI);

		pr_err(APN_NAME ": no answer in the shared block after %u ms\n",
			   jiffies_to_msecs(jiffies - start));

		switch (port) {
		case APN_ATTACH_NONE:
			pr_err(APN_NAME ": !PC never got the attach\n");
			break;
		case APN_ATTACH_PENDING:
			pr_err(APN_NAME ": !PC got the attach but hasn't carried "
				   "it out\n");
			break;
		case 1:
			pr_err(APN_NAME ": !PC attached, but its status word "
				   "never reached the PC (rings at %08lx: "
				   "cached?)\n", (unsigned long)p->phys);
			break;
		case APN_ATTACH_ADDRESS:
			pr_err(APN_NAME ": !PC can't reach the rings at %08lx\n",
				   (unsigned long)p->phys);
			break;
		default:
			pr_err(APN_NAME ": !PC refused the rings (%d), but its "
				   "status word never reached the PC\n", port);
		}
		return -EIO;
	}
	if (status != 1) {
		pr_err(APN_NAME ": !PC refused the rings (status %d)\n", status);
		return -EIO;
	}
	p->attached = true;
	return 0;
}

static void apn_detach(struct net_device *dev)
{
	struct apn_priv *p = netdev_priv(dev);

	if (!p->attached)
		return;
	p->attached = false;
	outw(APN_CMD_DETACH, dev->base_addr + APN_CMD);
}

/* ---- Sending ---- */

static inline u32 tx_room(struct apn_priv *p)
{
	return APN_TXSIZE - (sh_read(p, W_TXHEAD) - sh_read(p, W_TXTAIL));
}

static netdev_tx_t apn_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct apn_priv *p = netdev_priv(dev);
	unsigned long flags;
	u32 len = skb->len, head, word;

	if (len > APN_MAXFRAME || skb_linearize(skb)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&p->lock, flags);
	/* The queue is stopped while there's less than a whole record's
	   room, so this one fits. */
	head = sh_read(p, W_TXHEAD);
	word = len;
	ring_write(p->txring, APN_TXSIZE, head, &word, 4);
	ring_write(p->txring, APN_TXSIZE, head + 4, skb->data, len);
	wmb();
	head += (4 + len + 3) & ~3;
	sh_write(p, W_TXHEAD, head);
	/* !PC sets tx_busy on a kick and clears it only once it has looked
	   for more, so while it's set this frame will be found. */
	mb();
	if (!sh_read(p, W_TXBUSY))
		outw(head & 0xFFFF, dev->base_addr + APN_KICK);

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;

	if (tx_room(p) < APN_MAXREC) {
		/* Ask to be told when !PC has sent some, then look again in
		   case it already has. */
		sh_write(p, W_FLAGS, sh_read(p, W_FLAGS) | APN_F_TXSPACE);
		wmb();
		if (tx_room(p) < APN_MAXREC)
			netif_stop_queue(dev);
		else
			sh_write(p, W_FLAGS,
				 sh_read(p, W_FLAGS) & ~APN_F_TXSPACE);
	}
	spin_unlock_irqrestore(&p->lock, flags);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---- Receiving ---- */

static void apn_rx(struct net_device *dev)
{
	struct apn_priv *p = netdev_priv(dev);
	u32 head, tail = sh_read(p, W_RXTAIL);

	for (;;) {
		head = sh_read(p, W_RXHEAD);
		if (tail == head)
			break;
		rmb();
		while (tail != head) {
			u32 len = ring_word(p->rxring, APN_RXSIZE, tail) & 0xFFFF;
			u32 rec = (4 + len + 3) & ~3;
			struct sk_buff *skb;

			if (len < ETH_HLEN || len > APN_MAXFRAME + 4 ||
			    rec > head - tail) {
				netdev_warn(dev, "bad record (length %u), "
					    "receive ring emptied\n", len);
				dev->stats.rx_errors++;
				tail = head;
				break;
			}
			skb = netdev_alloc_skb_ip_align(dev, len);
			if (!skb) {
				dev->stats.rx_dropped++;
			} else {
				ring_read(skb_put(skb, len), p->rxring,
					  APN_RXSIZE, tail + 4, len);
				skb->protocol = eth_type_trans(skb, dev);
				netif_rx(skb);
				dev->stats.rx_packets++;
				dev->stats.rx_bytes += len;
			}
			tail += rec;
		}
		mb();
		sh_write(p, W_RXTAIL, tail);
	}
	/* !PC found the ring full and is holding frames back: say there's
	   room now. */
	if (sh_read(p, W_RXSTALLED)) {
		sh_write(p, W_RXSTALLED, 0);
		wmb();
		outw(APN_CMD_RXKICK, dev->base_addr + APN_CMD);
	}
}

static irqreturn_t apn_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct apn_priv *p = netdev_priv(dev);
	int cause;

	if (!p->attached)
		return IRQ_NONE;
	cause = inw(dev->base_addr + APN_KICK);

	if ((cause & APN_INT_TXSPACE) || netif_queue_stopped(dev)) {
		spin_lock(&p->lock);
		if (tx_room(p) >= APN_MAXREC) {
			sh_write(p, W_FLAGS,
				 sh_read(p, W_FLAGS) & ~APN_F_TXSPACE);
			netif_wake_queue(dev);
		}
		spin_unlock(&p->lock);
	}
	apn_rx(dev);
	return IRQ_HANDLED;
}

/* ---- The device ---- */

static void apn_set_rx_mode(struct net_device *dev)
{
	struct apn_priv *p = netdev_priv(dev);
	int rcr = RCR_BROADCAST;

	if (!p->attached)
		return;
	if (dev->flags & IFF_PROMISC) {
		rcr |= RCR_PROMISC;
	} else if ((dev->flags & IFF_ALLMULTI) || !netdev_mc_empty(dev)) {
		/* All multicasts: !PC filters with the 8390's hash. */
		sh_write(p, W_MCAST0, 0xFFFFFFFF);
		sh_write(p, W_MCAST1, 0xFFFFFFFF);
		rcr |= RCR_MULTICAST;
	}
	wmb();
	outw(rcr, dev->base_addr + APN_PARLO);
	outw(0, dev->base_addr + APN_PARHI);
	outw(APN_CMD_RXOPTS, dev->base_addr + APN_CMD);
}

static int apn_open(struct net_device *dev)
{
	int err;

	err = request_irq(dev->irq, apn_interrupt, 0, APN_NAME, dev);
	if (err) {
		pr_err(APN_NAME ": IRQ %d is busy\n", dev->irq);
		return err;
	}
	err = apn_attach(dev);
	if (err) {
		free_irq(dev->irq, dev);
		return err;
	}
	apn_set_rx_mode(dev);
	netif_start_queue(dev);
	return 0;
}

static int apn_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	apn_detach(dev);
	free_irq(dev->irq, dev);
	return 0;
}

static void apn_tx_timeout(struct net_device *dev)
{
	struct apn_priv *p = netdev_priv(dev);

	netdev_warn(dev, "send timed out (head %x, tail %x)\n",
		    sh_read(p, W_TXHEAD), sh_read(p, W_TXTAIL));
	dev->stats.tx_errors++;
	/* Nudge !PC again; it also looks at the rings from its poll. */
	outw(sh_read(p, W_TXHEAD) & 0xFFFF, dev->base_addr + APN_KICK);
	if (tx_room(p) >= APN_MAXREC)
		netif_wake_queue(dev);
}

static const struct net_device_ops apn_ops = {
	.ndo_open		= apn_open,
	.ndo_stop		= apn_stop,
	.ndo_start_xmit		= apn_start_xmit,
	.ndo_set_rx_mode	= apn_set_rx_mode,
	.ndo_tx_timeout		= apn_tx_timeout,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ---- Setting up ---- */

/* Which driver this is, in its messages, to tell builds apart. */
#define APN_REVISION	3

static struct net_device *apn_found;	/* tried, to be registered */

static void apn_free_rings(struct apn_priv *p)
{
	set_memory_wb(p->virt, APN_PAGES);
	free_pages(p->virt, APN_ORDER);
}

/*
 * Early, before ne.c probes (from net_olddevs_init, a device initcall):
 * if this !PC has the fast path, try it - rings, an attach, the Ethernet
 * address - and only if that works take the NE2000's ports too, so that
 * ne.c finds them busy. If anything fails, ne.c has the NE2000 as before.
 */
static int __init apn_claim(void)
{
	struct net_device *dev;
	struct apn_priv *p;

	if (disable)
		return 0;
	if (!request_region(io, APN_PORTS, APN_NAME))
		return 0;
	if (inw(io + APN_CMD) != APN_ID || inw(io + APN_PARLO) != APN_VERSION) {
		pr_info(APN_NAME " %d: no fast path at %#x; the NE2000 is "
			"ne.c's\n", APN_REVISION, io);
		goto out_region;
	}
	pr_info(APN_NAME " %d: fast path at %#x, trying it\n", APN_REVISION,
		io);

	dev = alloc_etherdev(sizeof(*p));
	if (!dev)
		goto out_region;
	p = netdev_priv(dev);
	spin_lock_init(&p->lock);

	p->virt = __get_free_pages(GFP_KERNEL, APN_ORDER);
	if (!p->virt)
		goto out_free;
	if (set_memory_uc(p->virt, APN_PAGES)) {
		pr_err(APN_NAME ": cannot make the rings uncached\n");
		set_memory_wb(p->virt, APN_PAGES);
		free_pages(p->virt, APN_ORDER);
		goto out_free;
	}
	/* Lines cached before the change must not be read stale later. */
	wbinvd();
	p->sh = (u32 *)p->virt;
	memset(p->sh, 0, APN_SIZE);
	p->phys = virt_to_phys(p->sh);
	p->txring = (u8 *)p->sh + APN_TXOFF;
	p->rxring = (u8 *)p->sh + APN_RXOFF;

	dev->base_addr = io;
	dev->irq = irq;
	dev->netdev_ops = &apn_ops;
	dev->watchdog_timeo = HZ;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_DATA_LEN;

	/* Attach once for the Ethernet address; open attaches again. */
	if (apn_attach(dev))
		goto out_rings;
	dev->dev_addr[0] = sh_read(p, W_MAC0);
	dev->dev_addr[1] = sh_read(p, W_MAC0) >> 8;
	dev->dev_addr[2] = sh_read(p, W_MAC0) >> 16;
	dev->dev_addr[3] = sh_read(p, W_MAC0) >> 24;
	dev->dev_addr[4] = sh_read(p, W_MAC1);
	dev->dev_addr[5] = sh_read(p, W_MAC1) >> 8;
	apn_detach(dev);

	if (!request_region(NE_IO, NE_PORTS, APN_NAME " (NE2000)")) {
		pr_warn(APN_NAME ": the NE2000's ports are taken\n");
		goto out_rings;
	}
	apn_found = dev;
	return 0;

out_rings:
	apn_free_rings(p);
out_free:
	free_netdev(dev);
out_region:
	release_region(io, APN_PORTS);
	pr_info(APN_NAME ": not used; the NE2000 is ne.c's\n");
	return 0;
}
fs_initcall(apn_claim);

static void apn_release(void)
{
	release_region(NE_IO, NE_PORTS);
	release_region(io, APN_PORTS);
}

static int __init apn_init(void)
{
	struct net_device *dev = apn_found;
	int err;

	if (!dev)
		return -ENODEV;
	apn_found = NULL;
	err = register_netdev(dev);
	if (err) {
		apn_free_rings(netdev_priv(dev));
		free_netdev(dev);
		apn_release();
		return err;
	}
	apn_dev = dev;
	netdev_info(dev, "!PC's NE2000 fast path at %#x, IRQ %d, rings at "
		    "%08lx, %pM\n", io, irq,
		    (unsigned long)((struct apn_priv *)netdev_priv(dev))->phys,
		    dev->dev_addr);
	return 0;
}

static void __exit apn_exit(void)
{
	struct apn_priv *p;

	if (!apn_dev)
		return;
	p = netdev_priv(apn_dev);
	unregister_netdev(apn_dev);	/* stops it, so !PC lets go */
	apn_free_rings(p);
	free_netdev(apn_dev);
	apn_release();
}

module_init(apn_init);
module_exit(apn_exit);

MODULE_AUTHOR("Julian Brown");
MODULE_DESCRIPTION("Aleph One !PC's NE2000, through its fast path");
MODULE_LICENSE("GPL");
