// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2019-2022, Stephan Gerhold <stephan@gerhold.net> */

#include <debug.h>
#include <dev/flash.h>
#include <lib/ptable.h>
#include <partition_parser.h>
#include <string.h>
#include <target.h>

#include <lk2nd/init.h>

static void partition_split_mmc(const char *base_name, const char *name,
				uint32_t num_blocks, bool end)
{
	struct partition_entry *backup, *base, *split;
	int index = partition_get_index(base_name);

	if (index == INVALID_PTN) {
		dprintf(CRITICAL, "%s partition not found (as base for %s)\n",
			base_name, name);
		return;
	}
	base = &partition_get_partition_entries()[index];

	if (base->size < num_blocks) {
		dprintf(CRITICAL, "%s partition has not enough space for %s (%llu < %u)\n",
			base_name, name, base->size, num_blocks);
		return;
	}

	backup = partition_allocate();
	if (backup) {
		memcpy(backup, base, sizeof(*backup));
		snprintf((char*)backup->name, sizeof(backup->name), "real_%s", base_name);
	} else {
		dprintf(CRITICAL, "Too many partitions, cannot backup %s partition entry\n",
			base_name);
	}

	split = partition_allocate();
	if (split) {
		memcpy(split, base, sizeof(*split));
		strlcpy((char*)split->name, name, sizeof(split->name));
		if (end)
			split->first_lba = split->last_lba - num_blocks + 1;
		else
			split->last_lba = split->first_lba + num_blocks - 1;
		split->size = num_blocks;
	} else {
		dprintf(CRITICAL, "Too many partitions, cannot add virtual %s partition\n",
			name);
	}

	if (end)
		base->last_lba -= num_blocks;
	else
		base->first_lba += num_blocks;
	base->size -= num_blocks;
}

static void partition_split_flash(struct ptable *ptable, const char *base_name,
				  const char *name, unsigned length, bool end)
{
	struct ptentry *base = ptable_find(ptable, base_name);

	if (!base) {
		dprintf(CRITICAL, "%s partition not found (as base for %s)\n",
			base_name, name);
		return;
	}

	if (base->length < length) {
		dprintf(CRITICAL, "%s partition has not enough space for %s (%u < %u)\n",
			base_name, name, base->length, length);
		return;
	}

	if (ptable_size(ptable) < MAX_PTABLE_PARTS) {
		char backup_name[MAX_PTENTRY_NAME];
		snprintf(backup_name, sizeof(backup_name), "real_%s", base_name);
		ptable_add(ptable, backup_name, base->start, base->length,
			    base->flags, base->type, base->perm);
	} else {
		dprintf(CRITICAL, "Too many partitions, cannot backup %s partition entry\n",
			base_name);
	}

	if (ptable_size(ptable) < MAX_PTABLE_PARTS) {
		unsigned start = base->start;
		if (end)
			start += base->length - length;

		ptable_add(ptable, (char*)name, start, length, base->flags,
			   base->type, base->perm);
	} else {
		dprintf(CRITICAL, "Too many partitions, cannot add virtual %s partition\n",
			name);
	}

	if (!end)
		base->start += length;
	base->length -= length;
}

#ifdef PROJECT_LK2ND_MI8916
static void partition_split_mmc_mi8916(uint32_t block_size)
{
#define VIRTUAL_BOOT_SIZE (48*1024*1024)
#define VIRTUAL_RECOVERY_SIZE (72*1024*1024)
#define VIRTUAL_METADATA_SIZE (32*1024*1024)
#define TOTAL_FIXED_VIRTUAL_SIZE (VIRTUAL_BOOT_SIZE + VIRTUAL_RECOVERY_SIZE + VIRTUAL_METADATA_SIZE)

	unsigned long long virtual_boot_blocks = VIRTUAL_BOOT_SIZE / block_size;
	unsigned long long virtual_recovery_blocks = VIRTUAL_RECOVERY_SIZE / block_size;
	unsigned long long virtual_metadata_blocks = VIRTUAL_METADATA_SIZE / block_size;
	unsigned long long total_fixed_virtual_blocks = TOTAL_FIXED_VIRTUAL_SIZE / block_size;

	int index;
	struct partition_entry *base, *real_boot, *real_recovery, *v_boot, *v_recovery, *v_metadata;
	unsigned long long next_first_lba;

	index = partition_get_index("cache");
	if (index == INVALID_PTN) {
		dprintf(CRITICAL, "base partition not found\n");
		return;
	}
	base = &partition_get_partition_entries()[index];

	index = partition_get_index("boot");
	if (index == INVALID_PTN) {
		dprintf(CRITICAL, "real boot partition not found\n");
		return;
	}
	real_boot = &partition_get_partition_entries()[index];

	index = partition_get_index("recovery");
	if (index == INVALID_PTN) {
		dprintf(CRITICAL, "real recovery partition not found\n");
		return;
	}
	real_recovery = &partition_get_partition_entries()[index];

	index = partition_get_index("metadata");
	if (index != INVALID_PTN) {
		dprintf(CRITICAL, "real metadata partition is found\n");
		return;
	}

	if (base->size < total_fixed_virtual_blocks) {
		dprintf(CRITICAL, "base partition has not enough space (%llu < %llu)\n",
			base->size, total_fixed_virtual_blocks);
		return;
	}

	next_first_lba = base->first_lba;

	v_boot = partition_allocate();
	if (!v_boot) goto err_partition_allocate;
	memcpy(v_boot, base, sizeof(*v_boot));
	snprintf((char*)real_boot->name, sizeof(real_boot->name), "lk2nd");
	snprintf((char*)v_boot->name, sizeof(v_boot->name), "boot");
	v_boot->size = virtual_boot_blocks;
	v_boot->first_lba = next_first_lba;
	v_boot->last_lba = v_boot->first_lba + v_boot->size - 1;
	next_first_lba = v_boot->last_lba + 1;

	v_recovery = partition_allocate();
	if (!v_recovery) goto err_partition_allocate;
	memcpy(v_recovery, base, sizeof(*v_recovery));
	snprintf((char*)real_recovery->name, sizeof(real_recovery->name), "lk2nd_recovery");
	snprintf((char*)v_recovery->name, sizeof(v_recovery->name), "recovery");
	v_recovery->size = virtual_recovery_blocks;
	v_recovery->first_lba = next_first_lba;
	v_recovery->last_lba = v_recovery->first_lba + v_recovery->size - 1;
	next_first_lba = v_recovery->last_lba + 1;

	v_metadata = partition_allocate();
	if (!v_metadata) goto err_partition_allocate;
	memcpy(v_metadata, base, sizeof(*v_metadata));
	snprintf((char*)v_metadata->name, sizeof(v_metadata->name), "metadata");
	v_metadata->size = virtual_metadata_blocks;
	v_metadata->first_lba = next_first_lba;
	v_metadata->last_lba = v_metadata->first_lba + v_metadata->size - 1;
	next_first_lba = v_metadata->last_lba + 1;

	base->first_lba = next_first_lba;
	base->size = base->last_lba - base->first_lba + 1;

	return;

err_partition_allocate:
	dprintf(CRITICAL, "failed to allocate partition\n");
}
#endif

static void lk2nd_partition_split_mmc(void)
{
	uint32_t block_size __UNUSED = mmc_get_device_blocksize();

#ifdef LK2ND_BOOT_PARTITION_SIZE
	partition_split_mmc(LK2ND_BOOT_PARTITION_BASE,
			    LK2ND_BOOT_PARTITION_NAME,
			    LK2ND_BOOT_PARTITION_SIZE / block_size, false);
#endif
#ifdef LK2ND_RECOVERY_PARTITION_SIZE
	partition_split_mmc(LK2ND_RECOVERY_PARTITION_BASE,
			    LK2ND_RECOVERY_PARTITION_NAME,
			    LK2ND_RECOVERY_PARTITION_SIZE / block_size, false);
#endif
#ifdef PROJECT_LK2ND_MI8916
	partition_split_mmc_mi8916(block_size);
#endif
}

static void lk2nd_partition_split_flash(void)
{
	struct ptable *ptable = flash_get_ptable();
	unsigned block_size __UNUSED = flash_block_size();

	if (!ptable)
		return;

#ifdef LK2ND_BOOT_PARTITION_SIZE
	partition_split_flash(ptable, LK2ND_BOOT_PARTITION_BASE,
			      LK2ND_BOOT_PARTITION_NAME,
			      LK2ND_BOOT_PARTITION_SIZE / block_size, false);
#endif
#ifdef LK2ND_RECOVERY_PARTITION_SIZE
	partition_split_flash(ptable, LK2ND_RECOVERY_PARTITION_BASE,
			      LK2ND_RECOVERY_PARTITION_NAME,
			      LK2ND_RECOVERY_PARTITION_SIZE / block_size, false);
#endif
}

static void lk2nd_device2nd_partition_split(void)
{
	if (target_is_emmc_boot())
		lk2nd_partition_split_mmc();
	else
		lk2nd_partition_split_flash();
}
LK2ND_INIT(lk2nd_device2nd_partition_split);
