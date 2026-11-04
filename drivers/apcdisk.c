// SPDX-License-Identifier: GPL-2.0
/*
 * apcdisk - Aleph One !PC's hard discs and swap device, through HPC.
 * For Linux 4.19 (the legacy request API, which went in 5.0).
 *
 * !PC, on the Risc PC's ARM, holds the PC's hard discs (image files or SCSI
 * drives, HD0 and HD1) and, from !PC 3.09, a swap device in a RISC OS
 * dynamic area. DOS reaches the discs through !PC's BIOS; a protected-mode
 * kernel cannot, so this driver talks to !PC directly with HPC (High-level
 * Procedure Call), as Aleph One's own Windows drivers do. From the DDK and
 * !PC's source (CPU.c.HPC, DEV.h.HPC_HD, DEV.h.HPC_SWP):
 *
 *   port 0x300  write: command; read: status
 *   port 0x302  16-bit write: a write-interlock token (HPC 1.1)
 *   port 0x304  data port for buffer A
 *   IRQ 15      raised when a call made with HPC_DO_CALL_IRQ completes
 *
 * A call is a packet of up to 16K in buffer A: the service ID, then whatever
 * the service wants. The PC fills the buffer, issues DO_CALL, !PC runs the
 * service and leaves the reply in the same buffer, and the PC reads it and
 * releases the buffer. Gemini cards also show the buffer in the PC's memory,
 * in the E0000-EFFFF window:
 *
 * - Replies are read from there. The buffer's pages are made uncached, as
 *   !PC writes them from the other side; the PC's cache kept stale copies.
 * - Requests go there only when !PC's HPC is 1.1 or later. The card's write
 *   FIFO can deliver the end of a packet after the DO_CALL, so the packet is
 *   followed by a token, as the buffer's last word and on port 0x302, which
 *   !PC waits to see. Older !PC is sent requests through the data port.
 *
 * Drives: apda and apdb are HD0 and HD1 (service 1); apdc is the swap device
 * (service 4), which has the hard disc service's packets.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include <asm/set_memory.h>
#include <asm/special_insns.h>

#define APD_NAME	"apd"

/* A block major from the ranges LANANA keeps for local use. */
static int major = 60;
module_param(major, int, 0444);
MODULE_PARM_DESC(major, "block major number (default 60)");

static bool noshm;
module_param(noshm, bool, 0444);
MODULE_PARM_DESC(noshm, "transfer through the data port only");

/* HPC ports, commands and status, as !PC's CPU.c.HPC defines them. */
#define HPC_PORT_FIRST	0x300
#define HPC_PORTS	8
#define HPC_CMD_PORT	(HPC_PORT_FIRST + 0)
#define HPC_STATUS_PORT	(HPC_PORT_FIRST + 0)
#define HPC_LOCK_PORT	(HPC_PORT_FIRST + 2)
#define HPC_DATAA_PORT	(HPC_PORT_FIRST + 4)
#define HPC_IRQ		15

#define HPC_WRITE_IO	0x00
#define HPC_READ_IO	0x10
#define HPC_WRITE_MEM	0x20
#define HPC_READ_MEM	0x30
#define HPC_DO_CALL	0x40
#define HPC_DO_CALL_IRQ	0x50
#define HPC_RELEASE_BUF	0x60
#define HPC_SUSPEND_ACK	0x82
#define HPC_BUFFER_A	0

#define HPCST_SUSPEND	0x80
#define HPCST_READY	0x20

#define HPC_BUF_LEN	0x4000

#define HPC_SYS_ID	0x0000
#define HPC_SYS_IDENT	0xEE435048
#define HPC_NO_SHM	0xFFFFFFFF

#define HPC_HD_ID	0x0001
#define HPC_SWAP_ID	0x0004
#define HPC_HD_INFO	0x0000
#define HPC_HD_READ	0x0001
#define HPC_HD_WRITE	0x0002
#define HDERR_OK	0

/* Packets are little-endian and naturally aligned, as the ARM lays them out. */
struct hpc_sysinfo_result {
	u32 identifier;
	u8 ver_minor, ver_major, pad1, pad2;
	u32 buf_a_physaddr;
	u32 buf_b_physaddr;
};

struct hd_info_params {
	u16 hpc_id, reason, drvnum;
};

struct hd_info_result {
	u16 status, ndrives;
	u32 drvsize, flags;
	u16 ncyls, nheads, nsects;
};

struct hd_rw_params {
	u16 hpc_id, reason, drvnum, sect_count;
	u32 sect_start;
	u16 readahead, pad2;
};

struct hd_rw_result {
	u16 status, pad2;
};

/* HD_MAX_RDWR_LEN in !PC: the buffer less room for the parameters. */
#define APD_MAX_SECTS	((HPC_BUF_LEN - 16) >> 9)

#define APD_HD_DRIVES	2
#define APD_SWAP_DRIVE	2
#define APD_MAX_DRIVES	3
#define APD_MINORS	16	/* the whole disc and 15 partitions */

/* How long a call may take before the request is failed. */
#define APD_TIMEOUT	(10 * HZ)

struct apd_drive {
	bool present;
	u16 service, drvnum;
	u32 nsects;
	u16 ncyls, nheads, nsects_per_track;
	struct gendisk *disk;
	struct request_queue *queue;
};

static struct apd_drive apd_drives[APD_MAX_DRIVES];

/*
 * Each drive has a queue, as each disc must; all share this lock. !PC runs
 * one call at a time, so one request is in progress over them all, and
 * they are served in turn.
 */
static DEFINE_SPINLOCK(apd_lock);
static int apd_next;			/* the drive to look at first */
static struct request *apd_busy;	/* the request a call is for */
static struct timer_list apd_timer;

static void __iomem *apd_shm;		/* buffer A where shared, or NULL */
static unsigned long apd_shm_virt;
static int apd_shm_npages;
static bool apd_interlock;		/* !PC waits for a shared request */
static u16 apd_token;

/* ---- HPC transport ---- */

static inline bool hpc_ready(void)
{
	return inb(HPC_STATUS_PORT) & HPCST_READY;
}

/* Each piece of a request's data, in order: its address and length. */
#define for_each_piece(req, iter, bv, p)				\
	rq_for_each_segment(bv, req, iter)				\
		if (((p) = kmap_atomic((bv).bv_page) + (bv).bv_offset), 1)

static void hpc_start_call(bool with_irq)
{
	outb((with_irq ? HPC_DO_CALL_IRQ : HPC_DO_CALL) | HPC_BUFFER_A,
	     HPC_CMD_PORT);
}

/*
 * Fill buffer A with a packet of parameters followed by a request's data (if
 * it is a write), and start the call. Lengths are even, since the data port
 * is driven a word at a time.
 */
static void hpc_send(const void *params, int plen, struct request *req,
		     bool with_irq)
{
	struct req_iterator iter;
	struct bio_vec bv;
	void *p;
	unsigned int off = plen;

	if (apd_shm && apd_interlock) {
		u16 token;

		apd_token += 2;
		if (!apd_token)
			apd_token = 2;
		token = apd_token | HPC_BUFFER_A;
		outb(HPC_WRITE_MEM | HPC_BUFFER_A, HPC_CMD_PORT);
		memcpy_toio(apd_shm, params, plen);
		if (req)
			for_each_piece(req, iter, bv, p) {
				memcpy_toio(apd_shm + off, p, bv.bv_len);
				off += bv.bv_len;
				kunmap_atomic(p);
			}
		wmb();
		writew(token, apd_shm + HPC_BUF_LEN - 2);
		wmb();
		outw(token, HPC_LOCK_PORT);
	} else {
		outb(HPC_WRITE_IO | HPC_BUFFER_A, HPC_CMD_PORT);
		outsw(HPC_DATAA_PORT, params, plen >> 1);
		if (req)
			for_each_piece(req, iter, bv, p) {
				outsw(HPC_DATAA_PORT, p, bv.bv_len >> 1);
				kunmap_atomic(p);
			}
	}
	hpc_start_call(with_irq);
}

/* Read a completed call's reply - a result, then data - and release A. */
static void hpc_receive(void *result, int rlen, struct request *req,
			void *data, int dlen)
{
	struct req_iterator iter;
	struct bio_vec bv;
	void *p;
	unsigned int off = rlen;

	if (apd_shm) {
		outb(HPC_READ_MEM | HPC_BUFFER_A, HPC_CMD_PORT);
		rmb();
		memcpy_fromio(result, apd_shm, rlen);
		if (data)
			memcpy_fromio(data, apd_shm + rlen, dlen);
		if (req)
			for_each_piece(req, iter, bv, p) {
				memcpy_fromio(p, apd_shm + off, bv.bv_len);
				off += bv.bv_len;
				flush_dcache_page(bv.bv_page);
				kunmap_atomic(p);
			}
	} else {
		outb(HPC_READ_IO | HPC_BUFFER_A, HPC_CMD_PORT);
		insw(HPC_DATAA_PORT, result, rlen >> 1);
		if (data)
			insw(HPC_DATAA_PORT, data, dlen >> 1);
		if (req)
			for_each_piece(req, iter, bv, p) {
				insw(HPC_DATAA_PORT, p, bv.bv_len >> 1);
				flush_dcache_page(bv.bv_page);
				kunmap_atomic(p);
			}
	}
	outb(HPC_RELEASE_BUF | HPC_BUFFER_A, HPC_CMD_PORT);
}

/* Wait, sleeping, for !PC to be ready. Only before the IRQ is ours. */
static int hpc_wait_ready(void)
{
	unsigned long end = jiffies + APD_TIMEOUT;

	while (!hpc_ready()) {
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		msleep(1);
	}
	return 0;
}

/* A whole call, waited for. For setting up, before the IRQ is ours. */
static int hpc_call_sync(const void *params, int plen, void *result, int rlen)
{
	if (hpc_wait_ready())
		return -ETIMEDOUT;
	hpc_send(params, plen, NULL, false);
	if (hpc_wait_ready())
		return -ETIMEDOUT;
	hpc_receive(result, rlen, NULL, NULL, 0);
	return 0;
}

/* ---- Requests ---- */

/* The next request from any drive's queue, each drive in turn. */
static struct request *apd_fetch(void)
{
	struct request *req;
	int i;

	for (i = 0; i < APD_MAX_DRIVES; i++) {
		struct apd_drive *d = &apd_drives[apd_next];

		apd_next = (apd_next + 1) % APD_MAX_DRIVES;
		if (d->queue && (req = blk_fetch_request(d->queue)) != NULL)
			return req;
	}
	return NULL;
}

/* Start a call for the next request, if none is in progress. Lock held. */
static void apd_start(void)
{
	struct hd_rw_params p;
	struct apd_drive *d;
	struct request *req;

	while (!apd_busy && (req = apd_fetch()) != NULL) {
		d = req->rq_disk->private_data;
		if (blk_rq_is_passthrough(req) ||
		    blk_rq_sectors(req) > APD_MAX_SECTS) {
			__blk_end_request_all(req, BLK_STS_IOERR);
			continue;
		}
		p.hpc_id = d->service;
		p.reason = rq_data_dir(req) == WRITE ? HPC_HD_WRITE
						     : HPC_HD_READ;
		p.drvnum = d->drvnum;
		p.sect_count = blk_rq_sectors(req);
		p.sect_start = blk_rq_pos(req);
		p.readahead = 0;
		p.pad2 = 0;

		/* The previous call is complete, so !PC is ready. */
		apd_busy = req;
		mod_timer(&apd_timer, jiffies + APD_TIMEOUT);
		hpc_send(&p, sizeof(p), rq_data_dir(req) == WRITE ? req : NULL,
			 true);
	}
}

static void apd_request(struct request_queue *q)
{
	apd_start();
}

static irqreturn_t apd_interrupt(int irq, void *dev_id)
{
	unsigned long flags;
	struct hd_rw_result r;
	struct request *req;
	int status = inb(HPC_STATUS_PORT);
	irqreturn_t ret = IRQ_NONE;

	/*
	 * !PC asks the PC to acknowledge a suspend before it freezes it, and
	 * on cards other than Gemini before it runs each call.
	 */
	if (status & HPCST_SUSPEND) {
		outb(HPC_SUSPEND_ACK, HPC_CMD_PORT);
		ret = IRQ_HANDLED;
	}

	spin_lock_irqsave(&apd_lock, flags);
	req = apd_busy;
	if (req && (status & HPCST_READY)) {
		del_timer(&apd_timer);
		hpc_receive(&r, sizeof(r),
			    rq_data_dir(req) == READ ? req : NULL, NULL, 0);
		if (r.status != HDERR_OK)
			pr_err(APD_NAME ": %s of %u at %llu failed, status %u\n",
			       rq_data_dir(req) == READ ? "read" : "write",
			       blk_rq_sectors(req),
			       (unsigned long long)blk_rq_pos(req), r.status);
		apd_busy = NULL;
		__blk_end_request_all(req, r.status == HDERR_OK ?
				      BLK_STS_OK : BLK_STS_IOERR);
		apd_start();
		ret = IRQ_HANDLED;
	}
	spin_unlock_irqrestore(&apd_lock, flags);
	return ret;
}

static void apd_timeout(struct timer_list *t)
{
	unsigned long flags;

	spin_lock_irqsave(&apd_lock, flags);
	if (apd_busy) {
		struct request *req = apd_busy;

		pr_err(APD_NAME ": no reply from !PC\n");
		/* Leave the buffer free for the next call. */
		outb(HPC_RELEASE_BUF | HPC_BUFFER_A, HPC_CMD_PORT);
		apd_busy = NULL;
		__blk_end_request_all(req, BLK_STS_IOERR);
		apd_start();
	}
	spin_unlock_irqrestore(&apd_lock, flags);
}

/* ---- Block device ---- */

static int apd_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	struct apd_drive *d = bdev->bd_disk->private_data;

	geo->cylinders = d->ncyls;
	geo->heads = d->nheads;
	geo->sectors = d->nsects_per_track;
	return 0;
}

static const struct block_device_operations apd_fops = {
	.owner	= THIS_MODULE,
	.getgeo	= apd_getgeo,
};

/* ---- Setting up ---- */

/* Buffer A, shared, at phys: uncached for as long as the driver uses it. */
static void apd_map(unsigned long phys)
{
	unsigned long first = phys & PAGE_MASK;
	unsigned long end = PAGE_ALIGN(phys + HPC_BUF_LEN);

	if (phys < 0xA0000 || phys + HPC_BUF_LEN > 0x100000) {
		pr_warn(APD_NAME ": buffer A at %08lx is outside 640K-1M, "
			"using the data port\n", phys);
		return;
	}
	/* The first MB is always in the kernel's direct map. */
	apd_shm_virt = (unsigned long)phys_to_virt(first);
	apd_shm_npages = (end - first) >> PAGE_SHIFT;
	if (set_memory_uc(apd_shm_virt, apd_shm_npages)) {
		pr_warn(APD_NAME ": cannot make buffer A uncached, "
			"using the data port\n");
		apd_shm_virt = 0;
		return;
	}
	/* Lines cached before the change go, so none is read stale later. */
	wbinvd();
	apd_shm = (void __iomem *)phys_to_virt(phys);
}

static void apd_unmap(void)
{
	if (apd_shm_virt)
		set_memory_wb(apd_shm_virt, apd_shm_npages);
	apd_shm_virt = 0;
	apd_shm = NULL;
}

static int __init apd_probe(void)
{
	u16 sysp[2] = { HPC_SYS_ID, 0 };
	struct hpc_sysinfo_result si;
	struct hd_info_params ip;
	struct hd_info_result ir;
	int i, found = 0;

	/* Nothing answers 0x300 as HPC does without !PC behind it. */
	if (inb(HPC_STATUS_PORT) == 0xff)
		return -ENODEV;
	if (hpc_call_sync(sysp, sizeof(sysp), &si, sizeof(si)) ||
	    si.identifier != HPC_SYS_IDENT)
		return -ENODEV;
	pr_info(APD_NAME ": !PC HPC %u.%02u, buffer A at %08x\n",
		si.ver_major, si.ver_minor, si.buf_a_physaddr);
	apd_interlock = si.ver_major > 1 ||
			(si.ver_major == 1 && si.ver_minor >= 1);

	if (!noshm && si.buf_a_physaddr != HPC_NO_SHM)
		apd_map(si.buf_a_physaddr);
	if (apd_shm)
		pr_info(APD_NAME ": replies through the shared buffer, "
			"requests %s\n", apd_interlock ?
			"too, with !PC's write interlock" :
			"through the data port (!PC has no write interlock)");

	for (i = 0; i < APD_MAX_DRIVES; i++) {
		bool swap = i == APD_SWAP_DRIVE;
		struct apd_drive *d = &apd_drives[i];

		ip.hpc_id = swap ? HPC_SWAP_ID : HPC_HD_ID;
		ip.reason = HPC_HD_INFO;
		ip.drvnum = swap ? 0 : i;
		if (hpc_call_sync(&ip, sizeof(ip), &ir, sizeof(ir)))
			return -EIO;
		if (ir.status == 0xffff) {
			if (!swap)
				return -ENODEV;	/* no hard disc service */
			continue;		/* a !PC without swap */
		}
		if (ir.status != HDERR_OK || ir.drvsize == 0)
			continue;
		d->present = true;
		d->service = ip.hpc_id;
		d->drvnum = ip.drvnum;
		d->nsects = ir.drvsize;
		d->ncyls = ir.ncyls;
		d->nheads = ir.nheads;
		d->nsects_per_track = ir.nsects;
		found++;
		pr_info(APD_NAME "%c: %s, %u MB\n", 'a' + i,
			swap ? "!PC swap" : (i ? "HD1" : "HD0"),
			ir.drvsize >> 11);
	}
	return found ? 0 : -ENODEV;
}

static int __init apd_init(void)
{
	int i, err;

	if (!request_region(HPC_PORT_FIRST, HPC_PORTS, APD_NAME))
		return -EBUSY;
	err = apd_probe();
	if (err)
		goto out_region;

	timer_setup(&apd_timer, apd_timeout, 0);
	err = request_irq(HPC_IRQ, apd_interrupt, 0, APD_NAME, NULL);
	if (err) {
		pr_err(APD_NAME ": IRQ %d is in use\n", HPC_IRQ);
		goto out_unmap;
	}

	err = register_blkdev(major, APD_NAME);
	if (err < 0)
		goto out_irq;
	if (major == 0)
		major = err;

	err = -ENOMEM;
	for (i = 0; i < APD_MAX_DRIVES; i++) {
		struct apd_drive *d = &apd_drives[i];
		struct gendisk *disk;

		if (!d->present)
			continue;
		d->queue = blk_init_queue(apd_request, &apd_lock);
		if (!d->queue)
			goto out_disks;
		blk_queue_logical_block_size(d->queue, 512);
		blk_queue_max_hw_sectors(d->queue, APD_MAX_SECTS);
		disk = alloc_disk(APD_MINORS);
		if (!disk)
			goto out_disks;
		disk->major = major;
		disk->first_minor = i * APD_MINORS;
		disk->fops = &apd_fops;
		disk->private_data = d;
		disk->queue = d->queue;
		snprintf(disk->disk_name, sizeof(disk->disk_name),
			 APD_NAME "%c", 'a' + i);
		set_capacity(disk, d->nsects);
		d->disk = disk;
	}
	for (i = 0; i < APD_MAX_DRIVES; i++)
		if (apd_drives[i].disk)
			add_disk(apd_drives[i].disk);
	return 0;

out_disks:
	for (i = 0; i < APD_MAX_DRIVES; i++) {
		if (apd_drives[i].disk)
			put_disk(apd_drives[i].disk);
		if (apd_drives[i].queue)
			blk_cleanup_queue(apd_drives[i].queue);
	}
	unregister_blkdev(major, APD_NAME);
out_irq:
	free_irq(HPC_IRQ, NULL);
out_unmap:
	apd_unmap();
out_region:
	release_region(HPC_PORT_FIRST, HPC_PORTS);
	return err;
}

static void __exit apd_exit(void)
{
	int i;

	for (i = 0; i < APD_MAX_DRIVES; i++)
		if (apd_drives[i].disk) {
			del_gendisk(apd_drives[i].disk);
			put_disk(apd_drives[i].disk);
			blk_cleanup_queue(apd_drives[i].queue);
		}
	unregister_blkdev(major, APD_NAME);
	del_timer_sync(&apd_timer);
	free_irq(HPC_IRQ, NULL);
	apd_unmap();
	release_region(HPC_PORT_FIRST, HPC_PORTS);
}

module_init(apd_init);
module_exit(apd_exit);

MODULE_AUTHOR("Julian Brown");
MODULE_DESCRIPTION("Aleph One !PC's hard discs and swap device, through HPC");
MODULE_LICENSE("GPL");
