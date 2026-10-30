#include "../console.h"
#include "../drivers/storage/mtk_ufs.h"
#include "ufs_test.h"

static unsigned char gpt_header[8192] __attribute__((aligned(4096)));

void ufs_test(void)
{
    int rc = mtk_ufs_init();
    if (rc) {
        logf("UFS TEST: init failed rc=%d", (unsigned long)(-rc));
        return;
    }

    rc = mtk_ufs_read_blocks(2, 0, 2, gpt_header);
    if (rc) {
        logf("UFS TEST: LUN2 read failed rc=%d", (unsigned long)(-rc));
        return;
    }

    if (gpt_header[4096] == 'E' && gpt_header[4097] == 'F' &&
        gpt_header[4098] == 'I' && gpt_header[4099] == ' ' &&
        gpt_header[4100] == 'P' && gpt_header[4101] == 'A' &&
        gpt_header[4102] == 'R' && gpt_header[4103] == 'T')
        logf("UFS TEST: LUN2 GPT signature OK");
    else
        logf("UFS TEST: LUN2 GPT signature invalid");
}
