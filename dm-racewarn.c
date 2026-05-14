#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define DM_MSG_PREFIX "racewarn"

struct racewarn_io {
	sector_t start;
	sector_t len;
	bool is_write;
	struct list_head node;
};

struct racewarn_per_bio_data {
	struct racewarn_io *tracked_io;
};

struct racewarn_c {
	struct dm_dev *dev;
	sector_t start;
	spinlock_t lock;
	struct list_head list;
	unsigned long long race_count;
};

static bool racewarn_overlaps(sector_t start_a, sector_t len_a,
			      sector_t start_b, sector_t len_b)
{
	sector_t end_a = start_a + len_a;
	sector_t end_b = start_b + len_b;

	return start_a < end_b && start_b < end_a;
}

static bool racewarn_conflicts(bool new_is_write, bool active_is_write)
{
	if (new_is_write)
		return true;

	return active_is_write;
}

static const char *racewarn_conflict_type(bool new_is_write,
					  bool active_is_write)
{
	if (new_is_write && active_is_write)
		return "write-after-write";

	if (new_is_write)
		return "write-after-read";

	return "read-after-write";
}

static sector_t racewarn_map_sector(struct dm_target *ti, sector_t bi_sector)
{
	struct racewarn_c *rc = ti->private;

	return rc->start + dm_target_offset(ti, bi_sector);
}

static void racewarn_warn_conflict(struct racewarn_c *rc, sector_t start,
				   sector_t len, bool new_is_write,
				   sector_t active_start, sector_t active_len,
				   bool active_is_write,
				   unsigned int total_conflicts)
{
	if (total_conflicts > 1) {
		DMWARN_LIMIT(
			"race (%s): %s [%llu,%llu) conflicts with %s [%llu,%llu) on %pg (total conflicts: %u)",
			racewarn_conflict_type(new_is_write, active_is_write),
			new_is_write ? "write" : "read",
			(unsigned long long)start,
			(unsigned long long)(start + len),
			active_is_write ? "write" : "read",
			(unsigned long long)active_start,
			(unsigned long long)(active_start + active_len),
			rc->dev->bdev, total_conflicts);
		return;
	}

	DMWARN_LIMIT(
		"race (%s): %s [%llu,%llu) conflicts with %s [%llu,%llu) on %pg",
		racewarn_conflict_type(new_is_write, active_is_write),
		new_is_write ? "write" : "read",
		(unsigned long long)start, (unsigned long long)(start + len),
		active_is_write ? "write" : "read",
		(unsigned long long)active_start,
		(unsigned long long)(active_start + active_len), rc->dev->bdev);
}

static int racewarn_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct racewarn_c *rc;
	unsigned long long tmp;
	char dummy;
	int ret;

	if (argc != 1 && argc != 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	rc = kzalloc(sizeof(*rc), GFP_KERNEL);
	if (!rc) {
		ti->error = "Cannot allocate racewarn context";
		return -ENOMEM;
	}

	if (argc == 2) {
		ret = -EINVAL;
		if (sscanf(argv[1], "%llu%c", &tmp, &dummy) != 1 ||
		    tmp != (sector_t)tmp) {
			ti->error = "Invalid backend sector";
			goto bad;
		}
		rc->start = tmp;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &rc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	spin_lock_init(&rc->lock);
	INIT_LIST_HEAD(&rc->list);

	ti->private = rc;
	ti->per_io_data_size = sizeof(struct racewarn_per_bio_data);
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->num_secure_erase_bios = 1;
	ti->num_write_zeroes_bios = 1;

	return 0;

bad:
	kfree(rc);
	return ret;
}

static void racewarn_dtr(struct dm_target *ti)
{
	struct racewarn_c *rc = ti->private;
	struct racewarn_io *tracked, *tmp;

	list_for_each_entry_safe(tracked, tmp, &rc->list, node) {
		list_del(&tracked->node);
		kfree(tracked);
	}

	dm_put_device(ti, rc->dev);
	kfree(rc);
}

static int racewarn_track_bio(struct dm_target *ti, struct bio *bio)
{
	struct racewarn_c *rc = ti->private;
	struct racewarn_per_bio_data *pbd;
	struct racewarn_io *tracked;
	struct racewarn_io *active;
	sector_t start;
	sector_t len;
	sector_t conflict_start = 0;
	sector_t conflict_len = 0;
	unsigned long flags;
	unsigned int conflicts = 0;
	bool conflict_is_write = false;
	bool has_conflict = false;
	bool is_write;

	pbd = dm_per_bio_data(bio, sizeof(*pbd));
	pbd->tracked_io = NULL;

	if (bio_op(bio) != REQ_OP_READ && bio_op(bio) != REQ_OP_WRITE)
		return 0;

	len = bio_sectors(bio);
	if (!len)
		return 0;

	start = racewarn_map_sector(ti, bio->bi_iter.bi_sector);
	is_write = bio_op(bio) == REQ_OP_WRITE;

	tracked = kmalloc(sizeof(*tracked), GFP_NOIO);
	if (!tracked)
		return -ENOMEM;

	tracked->start = start;
	tracked->len = len;
	tracked->is_write = is_write;
	INIT_LIST_HEAD(&tracked->node);

	spin_lock_irqsave(&rc->lock, flags);
	list_for_each_entry(active, &rc->list, node) {
		if (!racewarn_overlaps(start, len, active->start, active->len))
			continue;
		if (!racewarn_conflicts(is_write, active->is_write))
			continue;

		if (!has_conflict) {
			conflict_start = active->start;
			conflict_len = active->len;
			conflict_is_write = active->is_write;
			has_conflict = true;
		}

		conflicts++;
		rc->race_count++;
	}
	list_add_tail(&tracked->node, &rc->list);
	spin_unlock_irqrestore(&rc->lock, flags);

	if (has_conflict)
		racewarn_warn_conflict(rc, start, len, is_write, conflict_start,
				       conflict_len, conflict_is_write,
				       conflicts);

	pbd->tracked_io = tracked;

	return 0;
}

static int racewarn_map(struct dm_target *ti, struct bio *bio)
{
	struct racewarn_c *rc = ti->private;
	int ret;

	ret = racewarn_track_bio(ti, bio);
	if (ret)
		return DM_MAPIO_KILL;
	bio_set_dev(bio, rc->dev->bdev);
	bio->bi_iter.bi_sector =
		racewarn_map_sector(ti, bio->bi_iter.bi_sector);

	return DM_MAPIO_REMAPPED;
}

static int racewarn_end_io(struct dm_target *ti, struct bio *bio,
			   blk_status_t *error)
{
	struct racewarn_c *rc = ti->private;
	struct racewarn_per_bio_data *pbd;
	struct racewarn_io *tracked;
	unsigned long flags;

	if ((bio_op(bio) != REQ_OP_READ && bio_op(bio) != REQ_OP_WRITE) ||
	    !bio_sectors(bio))
		return DM_ENDIO_DONE;

	pbd = dm_per_bio_data(bio, sizeof(*pbd));
	tracked = pbd->tracked_io;
	if (!tracked)
		return DM_ENDIO_DONE;

	spin_lock_irqsave(&rc->lock, flags);
	list_del(&tracked->node);
	spin_unlock_irqrestore(&rc->lock, flags);

	kfree(tracked);
	pbd->tracked_io = NULL;

	return DM_ENDIO_DONE;
}

static void racewarn_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flags, char *result,
			    unsigned int maxlen)
{
	struct racewarn_c *rc = ti->private;
	unsigned int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("races=%llu", rc->race_count);
		break;
	case STATUSTYPE_TABLE:
		DMEMIT("%s %llu", rc->dev->name, (unsigned long long)rc->start);
		break;
	case STATUSTYPE_IMA:
		DMEMIT_TARGET_NAME_VERSION(ti->type);
		DMEMIT(",device_name=%s,start=%llu,races=%llu;", rc->dev->name,
		       (unsigned long long)rc->start, rc->race_count);
		break;
	}
}

static int racewarn_prepare_ioctl(struct dm_target *ti,
				  struct block_device **bdev)
{
	struct racewarn_c *rc = ti->private;

	*bdev = rc->dev->bdev;

	if (rc->start || ti->len != bdev_nr_sectors(rc->dev->bdev))
		return 1;

	return 0;
}

static int racewarn_iterate_devices(struct dm_target *ti,
				    iterate_devices_callout_fn fn, void *data)
{
	struct racewarn_c *rc = ti->private;

	return fn(ti, rc->dev, rc->start, ti->len, data);
}

static struct target_type racewarn_target = {
	.name = "racewarn",
	.version = { 1, 0, 0 },
	.features = DM_TARGET_PASSES_INTEGRITY | DM_TARGET_NOWAIT |
		    DM_TARGET_PASSES_CRYPTO,
	.module = THIS_MODULE,
	.ctr = racewarn_ctr,
	.dtr = racewarn_dtr,
	.map = racewarn_map,
	.end_io = racewarn_end_io,
	.status = racewarn_status,
	.prepare_ioctl = racewarn_prepare_ioctl,
	.iterate_devices = racewarn_iterate_devices,
};
module_dm(racewarn);

MODULE_AUTHOR("ArtemkaSheremetev");
MODULE_DESCRIPTION(DM_NAME " race detector target");
MODULE_LICENSE("GPL");
