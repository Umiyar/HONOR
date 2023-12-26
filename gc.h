/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fs/f2fs/gc.h
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#define GC_THREAD_MIN_WB_PAGES		1	/*
						 * a threshold to determine
						 * whether IO subsystem is idle
						 * or not
						 */
#define DEF_GC_THREAD_URGENT_SLEEP_TIME	500	/* 500 ms */
#define DEF_GC_THREAD_LOG_SLEEP_TIME	500
#define DEF_GC_THREAD_MIN_SLEEP_TIME	30000	/* milliseconds */
#define DEF_GC_THREAD_MAX_SLEEP_TIME	60000
#define DEF_GC_THREAD_NOGC_SLEEP_TIME	300000	/* wait 5 min */

/* choose candidates from sections which has age of more than 7 days */
#define DEF_GC_THREAD_AGE_THRESHOLD		(60 * 60 * 24 * 7)
#define DEF_GC_THREAD_CANDIDATE_RATIO		20	/* select 20% oldest sections as candidates */
#define DEF_GC_THREAD_MAX_CANDIDATE_COUNT	10	/* select at most 10 sections as candidates */
#define DEF_GC_THREAD_AGE_WEIGHT		60	/* age weight */
#define DEFAULT_ACCURACY_CLASS			10000	/* accuracy class */

#define LIMIT_INVALID_BLOCK	40 /* percentage over total user space */
#define LIMIT_FREE_BLOCK	40 /* percentage over invalid + free space */

#define DEF_GC_FAILED_PINNED_FILES	2048

/* Search max. number of dirty segments to select a victim segment */
#define DEF_MAX_VICTIM_SEARCH 4096 /* covers 8GB */
#define MAX_3(a, b, c) ((((a) < (b)) ? b : a) < (c)) ? c : ((a) < (b)) ? b : a

struct f2fs_gc_kthread {
	struct task_struct *f2fs_gc_task;
	wait_queue_head_t gc_wait_queue_head;

	/* for gc sleep time */
	unsigned int log_sleep_time;
	unsigned int urgent_sleep_time;
	unsigned int min_sleep_time;
	unsigned int max_sleep_time;
	unsigned int no_gc_sleep_time;

	/* for changing gc mode */
	unsigned int gc_wake;

	/* for GC_MERGE mount option */
	wait_queue_head_t fggc_wq;		/*
						 * caller of f2fs_balance_fs()
						 * will wait on this wait queue.
						 */
};

struct gc_inode_list {
	struct list_head ilist;
	struct radix_tree_root iroot;
};

struct victim_info {
	unsigned long long mtime;	/* mtime of section */
	unsigned int segno;		/* section No. */
};

struct victim_entry {
	struct rb_node rb_node;		/* rb node located in rb-tree */
	union {
		struct {
			unsigned long long mtime;	/* mtime of section */
			unsigned int segno;		/* segment No. */
		};
		struct victim_info vi;	/* victim info */
	};
	struct list_head list;
};

/*
 * inline functions
 */

/*
 * On a Zoned device zone-capacity can be less than zone-size and if
 * zone-capacity is not aligned to f2fs segment size(2MB), then the segment
 * starting just before zone-capacity has some blocks spanning across the
 * zone-capacity, these blocks are not usable.
 * Such spanning segments can be in free list so calculate the sum of usable
 * blocks in currently free segments including normal and spanning segments.
 */
static inline block_t free_segs_blk_count_zoned(struct f2fs_sb_info *sbi)
{
	block_t free_seg_blks = 0;
	struct free_segmap_info *free_i = FREE_I(sbi);
	int j;

	spin_lock(&free_i->segmap_lock);
	for (j = 0; j < MAIN_SEGS(sbi); j++)
		if (!test_bit(j, free_i->free_segmap))
			free_seg_blks += f2fs_usable_blks_in_seg(sbi, j);
	spin_unlock(&free_i->segmap_lock);

	return free_seg_blks;
}

static inline block_t free_segs_blk_count(struct f2fs_sb_info *sbi)
{
	if (f2fs_sb_has_blkzoned(sbi))
		return free_segs_blk_count_zoned(sbi);

	return free_segments(sbi) << sbi->log_blocks_per_seg;
}

static inline block_t free_user_blocks(struct f2fs_sb_info *sbi)
{
	block_t free_blks, ovp_blks;

	free_blks = free_segs_blk_count(sbi);
	ovp_blks = overprovision_segments(sbi) << sbi->log_blocks_per_seg;

	if (free_blks < ovp_blks)
		return 0;

	return free_blks - ovp_blks;
}

static inline block_t limit_invalid_user_blocks(block_t user_block_count)
{
	return (long)(user_block_count * LIMIT_INVALID_BLOCK) / 100;
}

static inline block_t limit_free_user_blocks(block_t reclaimable_user_blocks)
{
	return (long)(reclaimable_user_blocks * LIMIT_FREE_BLOCK) / 100;
}

static inline void increase_sleep_time(struct f2fs_gc_kthread *gc_th,
							unsigned int *wait)
{
	unsigned int min_time = gc_th->min_sleep_time;
	unsigned int max_time = gc_th->max_sleep_time;

	if (*wait == gc_th->no_gc_sleep_time)
		return;

	if ((long long)*wait + (long long)min_time > (long long)max_time)
		*wait = max_time;
	else
		*wait += min_time;
}

static inline void decrease_sleep_time(struct f2fs_gc_kthread *gc_th,
							unsigned int *wait)
{
	unsigned int min_time = gc_th->min_sleep_time;

	if (*wait == gc_th->no_gc_sleep_time)
		*wait = gc_th->max_sleep_time;

	if ((long long)*wait - (long long)min_time < (long long)min_time)
		*wait = min_time;
	else
		*wait -= min_time;
}

static inline bool has_enough_invalid_blocks(struct f2fs_sb_info *sbi)
{
	block_t user_block_count = sbi->user_block_count;
	block_t invalid_user_blocks = user_block_count -
		written_block_count(sbi);
	/*
	 * Background GC is triggered with the following conditions.
	 * 1. There are a number of invalid blocks.
	 * 2. There is not enough free space.
	 */
	return (invalid_user_blocks >
			limit_invalid_user_blocks(user_block_count) &&
		free_user_blocks(sbi) <
			limit_free_user_blocks(invalid_user_blocks));
}

static inline unsigned int area_of_log(struct f2fs_sb_info *sbi,int type){
	unsigned int num;
	printk("hello:111");
	switch(type){
		case 0:
			num = sbi->hi->log_end_blk[type] - sbi->hi->log_start_blk[type];
			break;
		case 1:
			num = sbi->hi->log_end_blk[type] - sbi->hi->log_start_blk[type];
			break;
		case 2:
			num = sbi->hi->log_end_blk[type] - sbi->hi->log_start_blk[type];
			break;
		default:
			num = __UINT32_MAX__;
			break;
	}
	return num;
}

static inline unsigned long valid_segment_sum(const unsigned long *addr, unsigned long size,unsigned int start){
	unsigned long idx;
	unsigned long temp;
	unsigned long mask;
	unsigned int offset;
	unsigned long ret;
	unsigned long basic;
	int i;
	if (small_const_nbits(size)) {
		unsigned long val = *addr | ~GENMASK(size - 1, 0);

		return val == ~0UL ? size : ffz(val);
	}
	mask = start/BITS_PER_LONG;
	offset = start%BITS_PER_LONG;
	temp = addr[mask];
	for(int i = 0;i < offset;i++){
		temp = temp | ((unsigned long)1<<i);
	}
	ret = 0;
	for (idx = mask; idx * BITS_PER_LONG < size; idx++) {
		i = 0;
		basic = 1;
		if(idx == mask){
			if (temp != ~0UL ){
				while(1){	
					i++;
					if(!(temp & basic))
						ret++;
					basic <<= 1;
					if(i>=64)
						break;
				}
			}
		}else{
			if (addr[idx] != ~0UL ){
				while(1){	
					i++;
					if(!(addr[idx] & basic))
						ret++;
					basic <<= 1;
					if(i>=64)
						break;
				}
			}
		}
	}
	return ret;
}

static inline bool log_need_GC_now(struct f2fs_sb_info *sbi,int *type)
{
	struct free_segmap_info *free_i = FREE_I(sbi);
	// struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct curseg_info *curseg;
	unsigned int valid_blks[3];
	unsigned int urgent;
	unsigned int cur_rate[3];
	unsigned int flag;
	// int flag1;
	unsigned long int seg_free;
	bool ret;
	for(int i = 0;i < 3;i++)
		valid_blks[i] = 0;
	for (int i = 0; i < MAIN_SEGS(sbi); i++) {
		int blks = get_seg_entry(sbi, i)->valid_blocks;
		int temp = get_seg_entry(sbi, i)->type;
		if (!blks)
			continue;
		if(temp == 0 || temp == 1 || temp == 2){
			valid_blks[temp] += blks;
		}
	}
	ret = false;
	// flag1 = 0;
	printk("hello:11");
	for(int i = 0;i < 3;i++){
		cur_rate[i]=0;
	}
	for(int i = 0;i < 3;i++){
		printk("invalid_counts:%u",sbi->hi->invalid_counts[i]);
		seg_free = valid_segment_sum(free_i->free_segmap,sbi->hi->log_end_blk[i],sbi->hi->log_start_blk[i]);
		printk("seg_free:%lu",valid_segment_sum(free_i->free_segmap,sbi->hi->log_end_blk[i],sbi->hi->log_start_blk[i]));
		printk("invalid:%u",sbi->hi->counts[i] - valid_blks[i]);
		curseg = CURSEG_I(sbi, i);
		printk("curseg->segno - sbi->gc_lastvic[i]:%u",(curseg->segno - sbi->gc_lastvic[i])*512);
		if(curseg->segno < sbi->gc_lastvic[i])
			flag = curseg->segno - sbi->gc_lastvic[i] + sbi->hi->log_end_blk[i]-sbi->hi->log_start_blk[i];
		else 
			flag = curseg->segno - sbi->gc_lastvic[i];
		printk("flag:%u",flag);
		if(flag*512 < 10 * (sbi->hi->log_end_blk[i]-sbi->hi->log_start_blk[i])*512 / 100 ){
			if(valid_segment_sum(free_i->free_segmap,sbi->hi->log_end_blk[i],sbi->hi->log_start_blk[i]) <= flag)
				printk("emergency situation");
			else
				continue;
		}
		if ((sbi->hi->invalid_counts[i])> 80 * (sbi->hi->log_end_blk[i]-sbi->hi->log_start_blk[i])*512 / 100)
		{	
			// if(i == 2 && sbi->hi->invalid_counts[2] < 50 *(sbi->hi->log_end_blk[2]-sbi->hi->log_start_blk[2])*512 / 100){
			// 	cur_rate[2] = 0;
			// 	// flag1 = 1;
			// }
			cur_rate[i] = (sbi->hi->invalid_counts[i])/seg_free;
			ret = true;
		}
	}
	for(int i = 0;i < 3;i++){
		printk("cur_rate[hot]:%u,cur_rate[warm]:%u,cur_rate[cold]:%u",cur_rate[0],cur_rate[1],cur_rate[2]);
	}
	// if(flag1 == 1){
	// 	*type = 2;
	// }
	if((cur_rate[0]!=0 || cur_rate[1]!=0 || cur_rate[2]!=0)){
		printk("cur_hot:%u,cur_warm:%u,cur_cold:%u",cur_rate[0],cur_rate[1],cur_rate[2]);	
		urgent = MAX_3(cur_rate[0],cur_rate[1],cur_rate[2]);
		for(int i = 0;i < 3;i++){
			if(urgent == cur_rate[i]){
				*type = i;
				printk("CURSEG_I(sbi, i):%d,log_area:%u",CURSEG_I(sbi, i)->segno,(sbi->hi->log_end_blk[i]-sbi->hi->log_start_blk[i]));
			}
		}
	}
	return ret;
}

static inline bool log_has_enough_invalid_blocks(struct f2fs_sb_info *sbi)
{	
	struct f2fs_stat_info *si;
	for(int type = 0;type < 3;type++){
		block_t log_block_count = sbi->hi->counts[type];
		block_t invalid_user_blocks = sbi->hi->counts[type] -
			si->valid_blks[type];
		printk("type:%d,log_block_count:%u,invalid_user_blocks:%u",type,log_block_count,invalid_user_blocks);
		if(invalid_user_blocks >limit_invalid_user_blocks(log_block_count) &&
			(area_of_log(sbi,type)*512 - log_block_count) < limit_free_user_blocks(invalid_user_blocks))
			return true;
	}
	return false;
}