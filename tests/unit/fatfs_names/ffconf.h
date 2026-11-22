// fatfs_names_test 用の FatFs の設定。ベアメタル版と同じ設定 (tools/ffconf_patch.sh で書き換えた ffconf_base.h。CMake が
// ビルドディレクトリに作る) を読み、ホストで動かすのに要る所だけ変える (排他は使わない、時計はない、f_mkfs を使う)
#include "ffconf_base.h"
#undef FF_FS_REENTRANT
#define FF_FS_REENTRANT 0
#undef FF_FS_NORTC
#define FF_FS_NORTC 1
#undef FF_USE_MKFS
#define FF_USE_MKFS 1
