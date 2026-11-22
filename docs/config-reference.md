# 設定の一覧

SD カードに置くファイル、pix68k.cfg のキー、cmdline.txt のオプションの詳しい説明。キーの短い一覧は [README](../README.md#pix68kcfg) の表にある。

## SD カードに置くファイル

ベアメタル版は、boot パーティション (か USB メモリ) の `/pix68k/` (ROM はルートでもよい) から次のファイルを読む。`pix68k.cfg` の行は、キーの説明をまとめて書いたもの。

| ファイル | 内容 |
| --- | --- |
| `IPLROM.DAT` | IPL ROM (128KB)。なければ X68000 は動かさず、[baremetal.md](baremetal.md) の M0 の動作をする |
| `IPLROM30.DAT` | X68030 の IPL ROM (128KB)。`pix68k.cfg` で `cpu=68030` のとき、`cpu=68060` で `IPLROM060.DAT` がないときに読む |
| `IPLROM060.DAT` | 060turbo の IPL ROM (128KB、なくてよい)。`cpu=68060` のときに先に探す ([060turbo.md](060turbo.md) を参照) |
| `CGROM.DAT` | CGROM (768KB)。なければ文字が出ない |
| `fd0.xdf`、`fd1.xdf` | フロッピーディスクのイメージ (`.dim`、`.d88` も可)。書き込みはファイルに書き戻す ([disks-and-scsi.md](disks-and-scsi.md) の「ディスクの書き戻しと大きなイメージ (ベアメタル版)」を参照) |
| `hd0.hdf` | SASI ID 0 のハードディスクのイメージ (HDF)。48MB まで (SASI の 40MB のドライブが入る)。書き込みはファイルに書き戻す |
| `SCSIEXROM.DAT` | 外付け SCSI ボードの ROM (なくてよい)。`pix68k.cfg` の `scsi_ex=on` のときに読む ([disks-and-scsi.md](disks-and-scsi.md) の「増設メモリ、外付け SCSI ボード、CD-ROM」を参照) |
| `pix68k.cfg` | 設定 (なくてよい)。`fd0=HUMAN302.XDF`、`hd0=HUMAN302.HDF` のように、ディスクのイメージの名前を指定する (`/` で始まらなければ `/pix68k` から)。`clock=10` で CPU を 10MHz にする (既定は `clock=16`、XVI)。`cpu=68030` で X68030 にする (IPL ROM は `IPLROM30.DAT`、SRAM は `SRAM30.DAT` / `SRAM30.BAK` を使う。`high_mem=16` で 16MB のハイメモリを置く)。`cpu=68060` で 060turbo にする (SRAM は `SRAM060.DAT` / `SRAM060.BAK`、`local_mem=` でローカルメモリの容量 (MB、0 から 256、既定 128)、`clock=75` で 68060 を 75MHz にする (既定は 50MHz。[060turbo.md](060turbo.md) の「060turbo (68060)」を参照))。`max_speed=on` で最高速モード (68000 でも使える。[060turbo.md](060turbo.md) の「最高速モード」を参照)。`speed060=` で 060turbo の命令の速さ (選んだクロックの実機の 68060 を 100 とした %、既定 100)、`max_speed_limit=` で 060turbo の最高速モードの上限 ([060turbo.md](060turbo.md) の「68060 のサイクル数」と「最高速モード」を参照)。`bus_sync_sprite=on` で、X68030 と 060turbo のスプライト ($EB0000-$EBFFFF) の読み書きも I/O と同じ 1.6 マイクロ秒ごとの区切りにそろえる (既定は `off`。測っていない仮定の値で、`on` にすると Outrun (CBX68) のスプライトが崩れる。[060turbo.md](060turbo.md) の「X68030 と 060turbo の本体のバス」)。`sram_memsize=keep` で SRAM に書かれたメインメモリの容量をそのまま使う (既定の `fix` では `ram=` の容量に合わせる)。`ram=8` で本体のメモリを 8MB にする (1 から 12)。`fpu_board=on` で数値演算プロセッサボード (`fpu_board=2` で 2 枚)、`mercury=on` でまーきゅりーゆにっとをつなぐ ([expansion.md](expansion.md) の「数値演算プロセッサボード、まーきゅりーゆにっと」を参照)。`neptune=on` でネットワークボード Neptune-X をつなぐ (`neptune_mac=`、`neptune_link=`。[expansion.md](expansion.md) の「ネットワークボード Neptune-X」を参照)。`rs232c=tcp:2324` で RS-232C を TCP の 2324 番につなぐ (`net=on` が要る。[expansion.md](expansion.md) の「RS-232C (SCC のチャネル A)」を参照)。`midi=on` で MIDI ボード CZ-6BM1 をつなぐ (`midi_sf2=` で SoundFont、`midi_out=`、`midi_voices=`、`midi_out=mt32` のときの `midi_mt32_ctrl=` と `midi_mt32_pcm=`、`midi_out=sc55` のときの `midi_sc55=`、`midi_out=uart` のときの `midi_uart=` と `midi_uart_family=`。[midi.md](midi.md) の「MIDI ボード CZ-6BM1 (YM3802)」を参照)。`net=on` で Pi の Ethernet に HTTP のサーバと syslog の送信を動かす (`net_ip=`、`net_token=`、`syslog=` など。[network-api.md](network-api.md) を参照)。`scsi_ex=on` で外付け SCSI ボードをつなぎ (ROM は `SCSIEXROM.DAT`。`scsi_ex_rom=` でファイル名を変えられる)、`sc0=HUMAN.HDS` から `sc6=` で SCSI のハードディスク (HDS)、`cd=GAME.ISO` か `cd=GAME.CUE` で CD-ROM (`cd_id=` で ID、既定 6) をつなぐ (外付け SCSI ボードがなければ X68030 / 060turbo の内蔵 SCSI。イメージは使うチャンクだけを読む。書き込みはファイルに書き戻す。[disks-and-scsi.md](disks-and-scsi.md) の「増設メモリ、外付け SCSI ボード、CD-ROM」と「ディスクの書き戻しと大きなイメージ (ベアメタル版)」を参照)。`write_back=off` でディスクの書き戻しをやめ、`write_protect=fd1,sc0` で書き込み禁止にするディスクを並べる。`keyboard=` でキーボードの種類を選ぶ (既定の `auto` ではつないだキーボードの VID:PID から決める。`usb` は普通の USB キーボード、`pi400us` / `pi400jp` は Raspberry Pi 400 の英語版 / 日本語版。[keyboard.md](keyboard.md) の「Pi 400 のキーボード」を参照)。`hat=0` / `hat=1` でゲームパッドのハットの値の範囲を 0-7 / 1-8 に固定する (既定の `auto` ではレポート記述子か値から決める。[baremetal.md](baremetal.md) を参照)。`pad_log=on` で、ゲームパッドのボタンとハットの生の値が変わるたびに `gamepad N: buttons=... hat=...` の行でログに出す (既定は `off`。配置の分からないゲームパッドの並びを調べるため。ログは USB の割り込みの中で書くので、調べ終えたら `off` に戻す)。`video_scale=off` で X68000 の画面を拡大しない (既定の `integer` では整数倍に拡大する。`aspect`、`full` と、`video_filter=` の効果は README の「画面の出し方」を参照)。`video_filter=scanline,curve,mask` (`,` で並べる) でブラウン管のように見せる (既定は `none`。それぞれスキャンライン、たる型のゆがみと四隅の暗さ、アパーチャーグリルとにじみで、自由に組み合わせられる。[baremetal.md](baremetal.md) の「画面の出し方」を参照。公開版 v0.1.0 の `scale=` と `scanline=` は読み替え、`crt=` は無視する)。`video_filter=` に `cz-600de` (グレー) か `cz-600db` (黒) を足すと、モニタの枠の絵 (`/pix68k/bezel-cz-600de.bin` など) を出し、X68000 の画面をそのガラスの部分に写す (フレームバッファが 1920x1080 のときだけ。README の「モニタの枠」と [baremetal.md](baremetal.md) の「モニタの枠」を参照)。画面の右上にフレームの数、速さ、温度、IP アドレスなどを出す (既定の `osd=on`。`osd=off` で出さない。ベアメタル版は余白が足りなければ `osd=on` でも X68000 の画面に重ねる (`osd=fps` との違いは、フレームバッファの高さが 148 ラインより低いときだけ)。ホスト版は `osd=on` では重ねず、`osd=fps` なら余白がないときに重ねる。[baremetal.md](baremetal.md) を参照)。`render_core=off` で画面の合成も、`sound_core=off` で音の合成もコア 1 で行う (既定はどちらも `on`。コア 2 / コア 3 が 1 秒以内に動き出さなければ、`on` でもコア 1 で行う。音を出さない設定なら、`sound_core=on` でもコア 3 は使わない) |
| `pix68k-<名前>.cfg` | 起動の組み合わせ (なくてよい)。1 つでもあれば起動時にメニューで選ぶ。`pix68k.cfg` からは共通の項目だけを受け継ぐ ([README](../README.md) の「起動の組み合わせ」と、下の「起動の組み合わせ」を参照) |
| `profile.sel` | 前回選んだ起動の組み合わせの名前 (pix68k が書く。`profile.new` は書き直すときの一時ファイル) |
| `SRAM.DAT`、`SRAM.BAK` | SRAM。起動時に読み、書き換えられたら書き戻す (5 秒に 1 回まで)。電源投入のたびに、SRAM に書かれたメインメモリの容量 ($ED0008) をエミュレータのメモリの容量に合わせる (XEiJ の説明書 parameter.htm の modifymemorysize と同じ働き。`pix68k.cfg` の `sram_memsize=keep` で止められる)。書いている途中で電源が切れても前の内容が残るよう、2 つのファイルに世代番号と CRC を付けて交互に書き、正しくて新しいほうを読む (形式は `src/x68k/sram_file.h`。先頭の 16KB は SRAM そのもので、16KB だけの SRAM.DAT も読める) |

## pix68k.cfg の書き方

pix68k.cfg のキーは `src/platform/settings.h` の `kSettingKeys` の表に 1 か所にまとめてある (ベアメタル版、ホスト版の `--config`、設定メニューが同じ表を使う)。
fd0 / fd1 / hd0 / sc0〜sc6 / cd / scsi_ex_rom はファイル名 (`/` で始まらなければ `/pix68k/` から)。
ファイル名には日本語も使える。`pix68k.cfg` は UTF-8 で書く (Shift_JIS で書いた名前は読み替えない)。ベアメタル版は SD カードのファイル名を UTF-8 で扱い、長い名前のない古い 8.3 の名前は CP932 (Shift_JIS) として読む。設定メニューのイメージの一覧は、日本語の名前を CGROM の字で出す (CGROM.DAT がなければ、全角は四角、半角カタカナは `?`)。一覧に出すのは、名前が拡張子を含めて 95 バイトまでのもの (UTF-8 の全角は 1 字 3 バイトなので、全角だけの名前なら、`.XDF` のような 4 バイトの拡張子を除いて 30 字まで)。前の版のベアメタル版で、HTTP の `PUT /files` から日本語の名前で置いたファイルは、SD カードに化けた名前で書かれているので、この版では名前が化けて見える。その場合は、消してから置き直す (PC で SD カードに書いた日本語の名前は、そのまま読める)。
clock (68000 では 10、16。68060 では 50、75)、cpu (68000、68030、68060)、ram (1〜12)、high_mem (0〜16)、local_mem (0〜256)、cd_id (0〜6)、speed060 (5〜3200)、max_speed_limit (0〜3200) は数。
autotype_wait (0〜600)、profile_wait (0〜60) も数。autotype は文字列 (`POST /type` と同じエスケープ)。
max_speed、bus_sync_sprite、render_core、sound_core、scsi_ex、write_back は on / off。write_protect はディスクの名前 (fd0 fd1 hd0 sc0〜sc6) を「,」か空白で区切って並べる。sram_memsize は fix / keep、hat は auto / 0 / 1、keyboard は auto / usb / pi400us / pi400jp。
知らないキーと正しくない値は、ログに警告を出して無視する。数のあとに文字が付いた値 (`high_mem=8MB` など) も正しくない値として無視する (前は数の部分だけを読んでいた)。
`fd0=` / `fd1=` を空で書くと、ドライブを空にする (`fd0.xdf` などを探さない。メニューで取り出してから「Save settings」を選ぶとこう書く)。ほかのファイル名のキー (`hd0=`、`sc0=` など) は、空なら指定しなかったのと同じ。

## 起動の組み合わせ

決まりは `src/platform/boot_profile.h`、ベアメタル版の処理は `baremetal/boot_menu.cpp` にある (#231)。

- 一覧に出すのは `/pix68k` の `pix68k-<名前>.cfg` のうち、名前が英数字と `-`、`_` だけ (32 字まで) のもの。ほかの名前のファイルはログに警告を出して飛ばす。名前の順に 30 個まで (`POST /reboot?profile=` は、一覧に入らなかった組み合わせも、ファイルがあれば使う)
- 保存の途中で止まって `pix68k-<名前>.new` (書き直すときの一時ファイル) だけが残った組み合わせも一覧に出す。選ぶと `.new` を読み、次に「Save settings」を選んだときに `.cfg` に戻る
- `POST /reboot?profile=` で `once` 付きで書いたあと、ディスクを書き戻せずに再起動を断ったら、`profile.sel` を前回の選択に書き戻す (戻せなかったときは、`profile=` なしの `POST /reboot`、Ctrl + Alt + Delete、設定メニューの Restart の前にもう一度戻し、それでも戻せなければ再起動しない。画面の左上に「再起動中止」と出す)。書けたか、戻せたかは、次の起動と同じ読み方で `profile.sel` (なければ `profile.new`) を読み直して決める
- メニューの題は、ファイルの 1 行目が `#` で始まれば、その残り (120 バイトまで)。全角と半角カタカナは CGROM の字で描くので、メニューを出すときは CGROM.DAT を先に読む (X68000 を用意するときはそれを使う)
  - 題の字は UTF-8 か Shift_JIS。1 行目が正しい UTF-8 なら UTF-8、そうでなければ Shift_JIS として読む (先頭に UTF-8 の BOM があれば UTF-8)。ただし、半角カタカナだけの題で、並びがたまたま正しい UTF-8 にもなるもの (`ﾂｰ` = 0xC2 0xB0 など) は、UTF-8 として読んで化ける。全角の字を混ぜるか、UTF-8 で書けば正しく出る。また、UTF-8 の題に壊れたバイトが 1 つでも混じると、題全体を Shift_JIS として読むので化ける (前はそのバイトだけが `?` になっていた)。題のほかの値 (ファイル名など) の読み方は変わらない
- `profile.sel` の 1 行目は名前 (空なら `pix68k.cfg` だけ)。`POST /reboot?profile=` のときは、要求を受けたときに 2 行目に `once` を付けて書き (書けなければ再起動しない)、次の起動はメニューを出さずにその組み合わせにして、`once` を消す (この起動で 1 回書く)。
  メニューで前回と同じものを選んだときと、時間が来て前回のものになったときは書かない
- 前回の組み合わせのファイルがなくなっていれば、`pix68k.cfg` を選んだ状態でメニューを出す
- メニューの間は、ログを画面に出さない (UART と `/log` には出る)。Ctrl + Alt + Delete で再起動する
- 決めたあと、決めるのに押したキーとボタンを離すまで (長くても 2 秒) 待ってから X68000 を動かす (Enter が X68000 に届かないように)
- 組み合わせの `keyboard=` か `hat=` が `pix68k.cfg` と違えば、メニューのためにつないだキーボードとゲームパッドに当て直す
- `autotype` は USB キーボードと同じ経路で打つ (`POST /type` と同じく、押して 40ms、離して 40ms)。設定メニューを開いている間は待つ。`autotype=` の値は 127 バイトまで (エスケープを戻す前)

## cmdline.txt のオプション

boot パーティションのルートの `cmdline.txt` (1 行) に、空白で区切って書く。Circle が読むもの (`width=` など) と、pix68k が読む `pix68k_` で始まるものがある。

| オプション | 内容 | 既定 | 詳しい説明 |
| --- | --- | --- | --- |
| `width=768 height=512` | フレームバッファの大きさ (Circle のオプション) | HDMI の解像度 | [baremetal.md](baremetal.md) |
| `pix68k_sound=` | 音の出し先。`pwm` (3.5mm のジャック、Raspberry Pi 5 は GPIO 12 / 13)、`i2s` (I2S の DAC)、`off`。`null` は試験用 | HDMI (QEMU 用のビルドは `off`) | [baremetal.md](baremetal.md) |
| `pix68k_i2s_dac=` | I2S の DAC の設定のしかた。`auto`、`none`、I2C の番地 (`0x4c` など) | `auto` | [baremetal.md](baremetal.md) の「I2S の DAC」 |
| `pix68k_fan=` | Pi 5 のファン。`auto` (温度で 4 段階)、`full` (いつも全開)、`off` (触らない) | `auto` | [baremetal.md](baremetal.md) |
| `pix68k_drive=` | `/pix68k` を読むドライブ。`auto`、`usb`、`sd`、`nvme` | `auto` | [baremetal.md](baremetal.md) の「USB メモリから読み書きする」 |
| `pix68k_id=` | USB メモリを選ぶとき、`/pix68k/id.txt` の値がこれと同じものを使う | なし | 同上 |
| `pix68k_usb_wait=` | USB メモリが出てくるのを待つ上限 (ミリ秒) | `2000` | 同上 |
| `pix68k_gpu_scale=` | `2` でフレームバッファを縦横 1/2 にして、2 倍の拡大を GPU に任せる。`1x2` で縦だけ | 使わない | [baremetal.md](baremetal.md) |
| `pix68k_ts6bga=1` | Pi 4 / 5 の第2 HDMI に TS-6BGA 互換 VRAM を出す。`1` は `$EE0000`、`2` は `$EF0000` | `0` (無効) | [ts6bga.md](ts6bga.md) |
| `pix68k_fb_rows=0` | 試験用。変わっていない行も毎回フレームバッファに書く (書く行を減らす前の動きと比べるため) | | [baremetal.md](baremetal.md) |
| `pix68k_wfe=0` | 試験用。コア 0 とコア 1 が待つ間、wfe で眠らせずに空回りで待つ (温度や速さを比べるため) | | [baremetal.md](baremetal.md) |
| `pix68k_net=` などの `pix68k_` + ネットワークのキー | `pix68k.cfg` の `net`、`net_ip`、`net_gw`、`net_dns`、`net_name`、`net_allow`、`net_token`、`syslog` と同じ。`cmdline.txt` のほうを使う (`rs232c` は読まない) | | [network-api.md](network-api.md) |
| `pix68k_textdump=N` | 試験用。N 秒ごとにテキスト画面を読み、変わっていればログに出す | | [baremetal.md](baremetal.md) |
| `pix68k_fail_core=N` | 試験用。N 番以降のコアが起動しなかったことにする | | [baremetal.md](baremetal.md) |
| `pix68k_net_probe=1` | 試験用。Neptune-X (`neptune_link=eth`) のリンクがつながったら、X68000 の MAC アドレスから ARP の問い合わせを出し、答えが届いたらログに出す (`baremetal/net_bridge.h`) | | |
| `pix68k_stream_min=0` | 試験用。SASI のイメージを全体を読まずにチャンクだけ読む (`tools/qemu_human68k.sh` の `STREAM=1`) | | [baremetal.md](baremetal.md) |
| `pix68k_deploy=秒数` | `tools/deploy_pi.sh` が使う。指定の秒数動いたらログを書いて再起動する | | [disks-and-scsi.md](disks-and-scsi.md)、[baremetal.md](baremetal.md) |

## ホスト版のコマンドラインのオプション

ホスト版の `pix68k` のオプションの一覧は `host/main.cpp` の先頭の注釈にある。よく使うものは [README](../README.md#ホスト版のオプション) に、
音、RTC の時刻、入力のオプションは [build-and-test.md](build-and-test.md) にある。`--config FILE` で `pix68k.cfg` と同じ形式の設定ファイルを読める
(コマンドラインのほうが優先する。README の表の「ホスト版」の列が「-」のキーは、ホスト版では黙って無視する)。
