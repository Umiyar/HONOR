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
    int type;
    type = -1;
    if(GET_SEGNO(fio->sbi, fio->old_blkaddr) == NULL_SEGNO){//第一次写入
		// printk("Now enter into cold");
        Native_info = __UINT32_MAX__;
        type = CURSEG_COLD_DATA;
        fio->temp = COLD;
        fio->sbi->hi->counts[fio->temp]++;
		fio->sbi->hi->new_blk_cnt++;
    }else{//更新写
		// printk("Native_info:%u",Native_info);
		fio->sbi->hi->upd_blk_cnt++;
        fio->sbi->hi->Native_set[fio->sbi->hi->hotness_num++] = Native_info;
		// printk("hotness_num:%u",fio->sbi->hi->hotness_num);
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
