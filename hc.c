#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>
#include <linux/random.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "hc.h"
#include "kmeans.h"
static void init_hc_management(struct f2fs_sb_info *sbi);
static int kmeans_thread_func(void *data);
static DEFINE_MUTEX(mutex_reduce_he);

int hotness_decide(struct f2fs_io_info *fio,__u32 Native_info){
	// printk("Native_info:%u",Native_info);
	// __u32 Native_info;
	unsigned int segno_old;
	int type_old;
	struct free_segmap_info *free_i = FREE_I(fio->sbi);
    int type;
    type = -1;
    if(GET_SEGNO(fio->sbi, fio->old_blkaddr) == NULL_SEGNO){//第一次写入
		// printk("Now enter into cold");
        Native_info = __UINT32_MAX__;
        type = CURSEG_COLD_DATA;
        fio->temp = COLD;
		if(valid_segment_sum_Noar(free_i->free_segmap,fio->sbi->hi->log_end_blk[COLD],fio->sbi->hi->log_start_blk[COLD]) < 100){
			printk("no cold space");
			if(valid_segment_sum_Noar(free_i->free_segmap,fio->sbi->hi->log_end_blk[WARM],fio->sbi->hi->log_start_blk[WARM] > 100)){
				fio->temp = WARM;
				type = CURSEG_WARM_DATA;
			}
			else{
				fio->temp = HOT;
				type = CURSEG_HOT_DATA;
			}
		}
		fio->sbi->hi->counts[fio->temp]++;
		fio->sbi->hi->new_blk_cnt++;
    }else{//更新写
		segno_old = GET_SEGNO(fio->sbi,fio->old_blkaddr);
		type_old = get_seg_entry(fio->sbi, segno_old)->type;
		fio->sbi->hi->invalid_counts[type_old]++;
		fio->sbi->hi->upd_blk_cnt++;
        fio->sbi->hi->Native_set[fio->sbi->hi->hotness_num++] = Native_info;
		if(fio->sbi->hi->hotness_num >= MAX_HOTNESS_ENTRY){
			fio->sbi->hi->hotness_num = 0;
			fio->sbi->hi->flag = 1;
			printk("The ceiling has been reached.");
		}
        if (fio->sbi->centers_valid) {//如果聚类完成
			type = kmeans_get_type(fio, Native_info);
		} else {
			type = fio->temp;
		}
		if(fio->sbi->hi->log_end_blk[type]!=0 && valid_segment_sum_Noar(free_i->free_segmap,fio->sbi->hi->log_end_blk[type],fio->sbi->hi->log_start_blk[type]) < 100){
			if(valid_segment_sum_Noar(free_i->free_segmap,fio->sbi->hi->log_end_blk[WARM],fio->sbi->hi->log_start_blk[WARM]) >= 50)
				type = CURSEG_WARM_DATA;
			else if(valid_segment_sum_Noar(free_i->free_segmap,fio->sbi->hi->log_end_blk[COLD],fio->sbi->hi->log_start_blk[COLD]) >= 50)
				type = CURSEG_COLD_DATA;
			else 
				type = CURSEG_HOT_DATA;
			fio->temp = type;
			fio->sbi->hi->counts[fio->temp]++;
			printk("No hot sapce;");
			return type;
		}
		if(valid_segment_sum_Noar(free_i->free_segmap,fio->sbi->hi->log_end_blk[type],fio->sbi->hi->log_start_blk[type]) < 50){
			if(valid_segment_sum_Noar(free_i->free_segmap,fio->sbi->hi->log_end_blk[WARM],fio->sbi->hi->log_start_blk[WARM]) > 100){
				fio->temp = WARM;
				type = CURSEG_WARM_DATA;
			}
			else if(valid_segment_sum_Noar(free_i->free_segmap,fio->sbi->hi->log_end_blk[COLD],fio->sbi->hi->log_start_blk[COLD]) > 100){
				fio->temp = COLD;
				type = CURSEG_COLD_DATA;
			}
			else{
				fio->temp = HOT;
				type = CURSEG_HOT_DATA;
			}
		}
        if (IS_HOT(type))
			fio->temp = HOT;
		else if (IS_WARM(type))
			fio->temp = WARM;
		else
			fio->temp = COLD;
        fio->sbi->hi->counts[fio->temp]++;
        fio->sbi->hi->Native_info_min[fio->temp] = MIN(fio->sbi->hi->Native_info_min[fio->temp], Native_info);
		fio->sbi->hi->Native_info_max[fio->temp] = MAX(fio->sbi->hi->Native_info_max[fio->temp], Native_info);
    }
    return type;
}

static void init_hc_management(struct f2fs_sb_info *sbi){
	int ret;
	struct file *fp;
	loff_t pos = 0;
	char buf[256];
	unsigned int n_clusters;
	unsigned int centers[3];
	unsigned int i;
	sbi->hi = f2fs_kmalloc(sbi, sizeof(struct hotness_info), GFP_KERNEL);
	sbi->hi->hotness_num = 0;
	sbi->hi->new_blk_cnt = 0;
	sbi->hi->upd_blk_cnt = 0;
	sbi->hi->flag = 0;
	sbi->hi->hc_count = 0;
	sbi->hi->warm_free_secmap = 0;
	sbi->hi->cold_free_secmap = 0;
	//--------------------------------------------------------------------
	sbi->hi->Native_set = vmalloc(sizeof(unsigned int) * MAX_HOTNESS_ENTRY);
	if (!sbi->hi->Native_set) {
        printk("In %s: data == NULL, count = %u.\n", __func__,sbi->hi->hotness_num);
    }
	else printk("In %s: success to malloc space", __func__);
	//--------------------------------------------------------------------
	for(i = 0; i < TEMP_TYPE_NUM; i++){
		sbi->hi->Native_info_min[i] = __UINT32_MAX__ ;
		sbi->hi->Native_info_max[i] = 0;
		sbi->hi->counts[i] = 0;
		sbi->hi->log_end_blk[i] = 0;
		sbi->hi->log_start_blk[i] = 0;
		sbi->gc_lastvic[i] = 0;
		sbi->hi->invalid_counts[0] = 0;
	}
	fp = filp_open("/tmp/f2fs_hotness_no", O_RDWR, 0664);
	if(IS_ERR(fp)){
		printk("No initial mass_center.\n");
		goto no_initial;
	}
	if(ret < 0){
		printk("kernel_read appear error.");
		goto no_initial;
	}
	sbi->n_clusters = N_CLUSTERS;
	// read centers
	for(i = 0; i < n_clusters; ++i) {
		memset(buf, 0, strlen(buf));
		kernel_read(fp, buf, strlen(buf), &pos);
		printk("read success!");
		sscanf(buf,"%u",&centers[i]);
		printk("buf:%s,n_clusters:%u",buf,centers[i]);
	}
	sbi->centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);
	sbi->centers = centers;
	sbi->centers_valid = 1;
	filp_close(fp, NULL);
	return;
no_initial:
	sbi->n_clusters = N_CLUSTERS;
	sbi->centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);
	sbi->centers_valid = 0;
	return;
}
void f2fs_build_hc_manager(struct f2fs_sb_info *sbi){
    init_hc_management(sbi);
}
static int kmeans_thread_func(void *data){
    struct f2fs_sb_info *sbi = data;
	struct f2fs_hc_kthread *hc_th = sbi->hc_thread;
	wait_queue_head_t *wq = &sbi->hc_thread->hc_wait_queue_head;
	int err;
	unsigned int total_blocks;
	unsigned int last_total_blocks;
	unsigned int wait_ms;
	wait_ms = hc_th->min_sleep_time;

	set_freezable();
	do {
		last_total_blocks = sbi->hi->new_blk_cnt + sbi->hi->upd_blk_cnt;

		wait_event_interruptible_timeout(*wq, kthread_should_stop() || freezing(current), msecs_to_jiffies(wait_ms));

		total_blocks = sbi->hi->new_blk_cnt + sbi->hi->upd_blk_cnt;

		if (total_blocks - last_total_blocks > DEF_HC_THREAD_DELTA_BLOCKS)
			hc_decrease_sleep_time(hc_th, &wait_ms);
		else
			hc_increase_sleep_time(hc_th, &wait_ms);

		err = f2fs_hc(sbi);
		if (!err) sbi->centers_valid = 1;
	} while (!kthread_should_stop());
	return 0;
}
int f2fs_start_hc_thread(struct f2fs_sb_info *sbi){
    struct f2fs_hc_kthread *hc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	int err = 0;

	hc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_hc_kthread), GFP_KERNEL);
	if (!hc_th) {
		err = -ENOMEM;
		goto out;
	}

	hc_th->min_sleep_time = DEF_HC_THREAD_MIN_SLEEP_TIME;
	hc_th->max_sleep_time = DEF_HC_THREAD_MAX_SLEEP_TIME;
	hc_th->no_hc_sleep_time = DEF_HC_THREAD_NOHC_SLEEP_TIME;

    sbi->hc_thread = hc_th;
	init_waitqueue_head(&sbi->hc_thread->hc_wait_queue_head);
    sbi->hc_thread->f2fs_hc_task = kthread_run(kmeans_thread_func, sbi,
			"f2fs_hc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(hc_th->f2fs_hc_task)) {
		err = PTR_ERR(hc_th->f2fs_hc_task);
		kfree(hc_th);
		sbi->hc_thread = NULL;
	}
out:
	return err;
}
void save_hotness_entry(struct f2fs_sb_info *sbi)
{
	int ret;
	char buf[256];
	struct file *fp;
	loff_t pos = 0;
	unsigned int i;
	if(sbi->centers_valid == 1){
		fp = filp_open("/tmp/f2fs_hotness", O_RDWR | O_CREAT, 0664);
		if (IS_ERR(fp)) goto out;
		memset(buf, 0, strlen(buf));
		printk("write clusters success.");
		if(ret < 0) {
			printk("kernel_write appears error.");
			goto out;
		}
		// save centers
		for(i = 0; i < sbi->n_clusters; i++) {
			sprintf(buf,"%u\n",sbi->centers[i]);
			kernel_write(fp, buf, strlen(buf), &pos);
			memset(buf, 0, strlen(buf));
		}
		filp_close(fp, NULL);
	}
out:
	return;
}
void f2fs_stop_hc_thread(struct f2fs_sb_info *sbi) {
    struct f2fs_hc_kthread *hc_th = sbi->hc_thread;
	
	if (!hc_th)
		return;
	kthread_stop(hc_th->f2fs_hc_task);
	kfree(hc_th);
	sbi->hc_thread = NULL;
}
void release_hotness_entry(struct f2fs_sb_info *sbi){
	if (sbi->centers) kfree(sbi->centers);
	if (sbi->hi->hotness_num == 0) return;
    vfree(sbi->hi->Native_set);
}

bool hc_can_inplace_update(struct f2fs_io_info *fio)
{
	unsigned int segno,segno_old;
	int type_blk;
	__u32 Native_info,new_blkaddr;
	struct curseg_info *curseg;
	segno_old = GET_SEGNO(fio->sbi,fio->old_blkaddr);
	type_blk = get_seg_entry(fio->sbi, segno_old)->type;
	curseg = CURSEG_I(fio->sbi, type_blk);
	new_blkaddr = NEXT_FREE_BLKADDR(fio->sbi, curseg);
	if (fio->type == DATA && fio->old_blkaddr != __UINT32_MAX__) {
		Native_info = new_blkaddr - fio->old_blkaddr;
	}
	if (type_blk != -1 && fio->sbi->centers_valid) {
		segno = GET_SEGNO(fio->sbi, new_blkaddr);
		if (segno_old == segno)	return true;
		else	return false;
	} else {
		return true;
	}
}


unsigned long find_log_first_zero_bit(const unsigned long *addr, unsigned long size,unsigned int start)
{
	unsigned long idx;
	unsigned long temp;
	unsigned long mask;
	unsigned int offset;
	if (small_const_nbits(size)) {
		unsigned long val = *addr | ~GENMASK(size - 1, 0);

		return val == ~0UL ? size : ffz(val);
	}
	mask = start/BITS_PER_LONG;
	offset = start%BITS_PER_LONG;
	printk("mask:%lu offset:%u",mask,offset);
	temp = addr[mask];
	printk("mask_old_temp:%lu",temp);
	for(int i = 0;i < offset;i++){
		temp = temp | ((unsigned long)1<<i);
		// printk("mask_temp:%lu",temp);
	}
	printk("mask_new_temp:%lu",temp);
	printk("mask------------------------------------------------------");
	for (idx = mask; idx * BITS_PER_LONG < size; idx++) {
		if(idx == mask){
			if (temp != ~0UL )
				return min(idx * BITS_PER_LONG + ffz(temp), size);
		}else{
			if (addr[idx] != ~0UL ){
				return min(idx * BITS_PER_LONG + ffz(addr[idx]), size);
			}
		}
	}
	return size;
}

