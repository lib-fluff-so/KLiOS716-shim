#ifndef KLIOS716_MTK_UFS_H
#define KLIOS716_MTK_UFS_H
typedef unsigned int u32;

int mtk_ufs_init(void);

/* KL7 main UFS LUN uses 4096-byte blocks. Buffer must be physically contiguous. */
int mtk_ufs_read_blocks(u32 lun, u32 lba, u32 count, void *buffer);
u32 mtk_ufs_block_size(void);

#endif
