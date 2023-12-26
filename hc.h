#ifndef _LINUX_HC_H
#define _LINUX_HC_H

#include <linux/timex.h>
#include <linux/workqueue.h>    /* for work queue */
#include <linux/slab.h>         /* for kmalloc() */

#define DEF_HC_THREAD_MIN_SLEEP_TIME	120000	/*    2 mins     */
#define DEF_HC_THREAD_MAX_SLEEP_TIME	3840000 /*    64 mins   */
#define DEF_HC_THREAD_NOHC_SLEEP_TIME	300000	/* wait 5 min */

#define DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD 1000000
#define DEF_HC_HOTNESS_ENTRY_SHRINK_NUM 100000
#define DEF_HC_THREAD_DELTA_BLOCKS		100000
#define MAX_HOTNESS_ENTRY 100000

#define MIN(a, b) ((a) < (b)) ? a : b
#define MAX(a, b) ((a) < (b)) ? b : a
struct f2fs_hc_kthread {
	struct task_struct *f2fs_hc_task;
	wait_queue_head_t hc_wait_queue_head;

	/* for hc sleep time */
	unsigned int min_sleep_time;
	unsigned int max_sleep_time;
	unsigned int no_hc_sleep_time;
};

int hotness_decide(struct f2fs_io_info *fio,__u32 Native_info);
void hotness_maintain(struct f2fs_io_info *fio, int type_old, int type_new, __u64 value);
void save_hotness_entry(struct f2fs_sb_info *sbi);
void release_hotness_entry(struct f2fs_sb_info *sbi);
bool hc_can_inplace_update(struct f2fs_io_info *fio);
unsigned long find_log_first_zero_bit(const unsigned long *addr, unsigned long size,unsigned int start);
// int get_victim_by_Noar(struct f2fs_sb_info *sbi,
// 			unsigned int *result, int gc_type, int type,
// 			char alloc_mode, unsigned long long age);
// void select_policy_Nora(struct f2fs_sb_info *sbi, int gc_type,
// 			int type, struct victim_sel_policy *p);
static inline void hc_decrease_sleep_time(struct f2fs_hc_kthread *hc_th, unsigned int *wait)
{
	unsigned int min_time = hc_th->min_sleep_time;
	if ((long long)((*wait)>>1) <= (long long)min_time)
		*wait = min_time;
	else
		*wait = ((*wait)>>1);
}

static inline void hc_increase_sleep_time(struct f2fs_hc_kthread *hc_th, unsigned int *wait)
{
	unsigned int max_time = hc_th->max_sleep_time;

	if ((long long)((*wait)<<1) >= (long long)max_time)
		*wait = max_time;
	else
		*wait = ((*wait)<<1);
}

static inline unsigned long valid_segment_sum_Noar(const unsigned long *addr, unsigned long size,unsigned int start){
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

#endif