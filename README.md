# pix68k
Bare Metal X68000 emulator for Raspberry Pi

pix68k emulates the Sharp X68000 on Raspberry Pi 3 / 4 / 5 without an OS
(bare metal, built on Circle). Besides the 68000 models, it runs X68030
(68030 + 68882) and 060turbo (68060) configurations. A host build (SDL2)
for macOS and Linux is used for development and testing.
IPL ROM, CGROM and disk images are not included; use your own.
The documentation is in Japanese. License: GPLv3.

## pix68k について

シャープの X68000 を Raspberry Pi の上で、OS を使わずに (ベアメタルで) 動かすエミュレータ。低レベル部分 (起動、USB、HDMI、SD カード、割り込み、ネットワーク) は [Circle](https://github.com/rsta2/circle) を使う。

- 動く Raspberry Pi:
  - Raspberry Pi 3 Model B+ (実機で確かめている)。Raspberry Pi 3 Model B も同じカーネルで動かすが、実機では確かめていない
  - Raspberry Pi 4 Model B (実機で確かめている)
  - Raspberry Pi 400 (Raspberry Pi 4 Model B と同じカーネルで動かすが、実機では確かめていない)
  - Raspberry Pi 5 (実機で確かめている)
  - QEMU の raspi3b でも動く (試験用)
- 機種: X68000 (68000、10MHz / 16MHz)、X68030 (68030 25MHz と 68882)、060turbo (X68030 に 68060 50MHz / 75MHz を載せた構成)
- 周辺機器: FD、SASI と SCSI のハードディスク、CD-ROM、OPM と ADPCM の音、マウス、ジョイスティック、数値演算プロセッサボード、
  まーきゅりーゆにっと、ネットワークボード Neptune-X、RS-232C、MIDI ボード CZ-6BM1 (SoundFont、MT-32、SC-55mkII、実機の音源。MIDI は動作を保証しない試験的な機能)
- ホスト版: macOS と Linux で SDL2 を使って動く版。開発と試験に使う ([ソースからビルドする](#ソースからビルドする))

画面の合成をコア 2、音の合成をコア 3 で行い、X68000 そのものはコア 1 で実時間で動かす。フレームは飛ばさない。

## 始め方

1. [Releases](https://github.com/j3tm0t0/pix68k/releases) から `pix68k-<版>.zip` をダウンロードする
2. SD カードを FAT32 でフォーマットし (MBR の第 1 パーティション)、zip を展開した中身をすべて SD カードのルートにコピーする
3. SD カードの `pix68k/` フォルダに、IPL ROM、CGROM、ディスクのイメージを置く (下の「用意するもの」)
4. SD カードを Raspberry Pi に挿し、HDMI のモニタと USB のキーボードをつないで電源を入れる。HDMI に X68000 の画面が出る

zip には、Raspberry Pi 3 / 4 / 400 / 5 用のカーネル、Raspberry Pi のファームウェア、`config.txt`、`cmdline.txt` が入っている。
3 つのカーネルを同じ SD カードに入れてあり、`config.txt` の `[pi3]` `[pi3+]` `[pi4]` `[pi5]` の節で、機種に合うものをファームウェアが選ぶ。
FD0 から起動するには、`pix68k/fd0.xdf` を置くか、`pix68k/pix68k.cfg` にイメージの名前を書く。

```
# pix68k/pix68k.cfg の例 (1 行に 1 つ「キー=値」。# から行末までは注釈)
fd0 = HUMAN302.XDF
hd0 = HUMAN302.HDF
cpu = 68000
clock = 16
```

`fd0=` を書かなければ `pix68k/fd0.xdf` (`.dim`、`.d88` も) を、`hd0=` を書かなければ `pix68k/hd0.hdf` を使う。
X68000 が書き込んだ内容は、ディスクのイメージのファイルに書き戻す (`write_back=off` でやめる)。

### 用意するもの

IPL ROM、CGROM、ディスクのイメージは zip にもリポジトリにも入っていない。自分で用意する。

| ファイル | 要るとき | 内容 |
| --- | --- | --- |
| `IPLROM.DAT` | X68000 (68000) | IPL ROM (128KB)。なければ X68000 は動かさず、ファイルの一覧と入力をログに出してカラーバーを描く |
| `CGROM.DAT` | いつも | CGROM (768KB)。なければ文字が出ない |
| `IPLROM30.DAT` | X68030、060turbo | X68030 の IPL ROM (128KB)。XEiJ の `data/IPLROM30.DAT` が使える ([x68030.md](docs/x68030.md)) |
| `IPLROM060.DAT` | 060turbo (なくてよい) | 68060 で動く IPL ROM。なければ `IPLROM30.DAT` を直して使う ([060turbo.md](docs/060turbo.md)) |
| `SCSIEXROM.DAT` | 外付け SCSI ボード | 外付け SCSI ボードの ROM ([disks-and-scsi.md](docs/disks-and-scsi.md)) |
| ディスクのイメージ | | FD (`.xdf`、`.dim`、`.d88`)、SASI の HD (`.hdf`)、SCSI の HD (`.hds`)、CD-ROM (`.iso`、`.cue`) |
| 音源の ROM、SoundFont | MIDI ボード (試験的) | MT-32 / CM-32L、SC-55mkII の ROM か SF2 ファイル ([midi.md](docs/midi.md)) |

置き場所:

- ベアメタル版: SD カード (か USB メモリ) の FAT のパーティションの `/pix68k/`。ROM はルートに置いてもよい。SRAM とステートもここに書く
- ホスト版: ROM は `~/.cache/pix68k/rom` (`-r` で変える)。ディスクのイメージはどこでもよい (`-0`、`-h` などで渡す)

### 起動したあと

- 音は HDMI に出す (`cmdline.txt` で 3.5mm のジャックや I2S の DAC にも変えられる)
- USB のキーボード、マウス、ゲームパッド (2 台まで) が使える。操作は下の「操作」
- ログは UART (115200bps) に出る。Raspberry Pi 3 / 4 / 400 は GPIO 14/15、Raspberry Pi 5 は HDMI 端子の間の 3 ピンの UART 端子
- SD カードがないか `/pix68k` がないときは、USB メモリの最初の FAT のパーティションの `/pix68k/` を使う
  (Linux を入れた USB メモリなら `/boot/firmware/pix68k/`)。カーネルを LAN から起動して、SD カードを挿さずに使うこともできる
- Raspberry Pi 5 では、ファンのコネクタのファンを温度に合わせて回す
- 画面の右上に、fps、速さ、温度、IP アドレスなどを出す (`osd=`)

詳しくは [baremetal.md](docs/baremetal.md) を参照。

## オプション

### pix68k.cfg

ベアメタル版は `/pix68k/pix68k.cfg`、ホスト版は `--config` で渡したファイルを読む。1 行に 1 つ `キー=値` を書く。
ファイル名は `/` で始まらなければ `/pix68k/` (ホスト版は設定ファイルのディレクトリ) から探す。
知らないキーと正しくない値は、ログに警告を出して無視する。「ホスト版」の列が「-」のキーは、ホスト版では無視する。
詳しい説明は [config-reference.md](docs/config-reference.md) と、各行の右の資料にある。

| キー | 値 | 既定 | 内容 | ホスト版 |
| --- | --- | --- | --- | --- |
| `fd0`、`fd1` | ファイル名 | ベアメタル版は `fd0.xdf` などを探す。ホスト版はなし | フロッピーディスクのイメージ。空で書くとドライブを空にする | ○ |
| `hd0` | ファイル名 | ベアメタル版は `hd0.hdf`。ホスト版はなし | SASI ID 0 のハードディスク (ベアメタル版は 48MB まで) ([disks-and-scsi.md](docs/disks-and-scsi.md)) | ○ |
| `cpu` | `68000`、`68030`、`68060` | `68000` | 機種。68030 は X68030、68060 は 060turbo | ○ |
| `clock` | `10`、`16` / `50`、`75` | `16` / `50` | CPU のクロック (MHz)。68000 は 10 か 16、68060 は 50 か 75。68030 は 25 で固定 | ○ |
| `ram` | `1`〜`12` | `4` | 本体のメモリ (MB) | ○ |
| `high_mem` | `0`〜`16` | `0` | X68030 のハイメモリ (MB) ([x68030.md](docs/x68030.md)) | ○ |
| `local_mem` | `0`〜`256` | `128` | 060turbo のローカルメモリ (MB) ([060turbo.md](docs/060turbo.md)) | ○ |
| `max_speed` | `on`、`off` | `off` | 最高速モード。実時間に間に合う範囲で CPU をいちばん速く動かす ([060turbo.md](docs/060turbo.md)) | ○ |
| `speed060` | `5`〜`3200` | `100` | 060turbo の命令の速さ (選んだクロックの実機の 68060 を 100 とした %) | ○ |
| `max_speed_limit` | `0`〜`3200` | `0` (上限なし) | 060turbo の最高速モードの上限 (`speed060` と同じ単位) | ○ |
| `bus_sync_sprite` | `on`、`off` | `off` | X68030 と 060turbo で、スプライト ($EB0000-$EBFFFF) の読み書きも I/O と同じ 1.6 マイクロ秒ごとの区切りにそろえる。測っていない仮定の値で、`on` にすると Outrun (CBX68) のスプライトが崩れる ([060turbo.md](docs/060turbo.md)) | ○ |
| `sram_memsize` | `fix`、`keep` | `fix` | `fix` は起動のたびに SRAM のメインメモリの容量を `ram` に合わせる | ○ |
| `scsi_ex` | `on`、`off` | `off` | 外付け SCSI ボード ([disks-and-scsi.md](docs/disks-and-scsi.md)) | ○ |
| `scsi_ex_rom` | ファイル名 | `SCSIEXROM.DAT` | 外付け SCSI ボードの ROM | ○ |
| `sc0`〜`sc6` | ファイル名 | なし | SCSI のハードディスクのイメージ (HDS)。外付け SCSI ボードがなければ X68030 / 060turbo の内蔵 SCSI | ○ |
| `cd` | ファイル名 | なし | CD-ROM のイメージ (ISO か CUE) | ○ |
| `cd_id` | `0`〜`6` | `6` | CD-ROM をつなぐ SCSI ID | ○ |
| `write_back` | `on`、`off` | `on` | X68000 が書いた内容をイメージのファイルに書き戻す | - |
| `write_protect` | `fd0 fd1 hd0 sc0`〜`sc6` | なし | 書き込み禁止にするディスクを「,」か空白で区切って並べる | - |
| `fpu_board` | `off`、`on`、`2` | `off` | 数値演算プロセッサボード。`2` で 2 枚 ([expansion.md](docs/expansion.md)) | ○ |
| `mercury` | `on`、`off` | `off` | まーきゅりーゆにっと | ○ |
| `neptune` | `on`、`off` | `off` | ネットワークボード Neptune-X | ○ |
| `neptune_mac` | MAC アドレス、`pi` (ベアメタル版だけ) | `02:68:6b:00:00:01` | Neptune-X の MAC アドレス。`pi` は Raspberry Pi の MAC アドレスを使う。ホスト版で `pi` を書くと、`neptune=on` のときは正しくない MAC アドレスとして終了する | ○ |
| `neptune_link` | `eth`、`echo` など | ベアメタル版は `eth`、ホスト版は `echo` | Neptune-X のフレームの送受信先 | ○ |
| `rs232c` | `off`、`tcp:PORT` | `off` | RS-232C を TCP につなぐ。ベアメタル版は `net=on` が要る | ○ |
| `midi` | `on`、`off` | `off` | MIDI ボード CZ-6BM1。動作を保証しない試験的な機能 ([midi.md](docs/midi.md)) | ○ |
| `midi_out` | `sf2`、`mt32`、`sc55`、`uart`、`none` | `sf2` | MIDI の行き先。UART への実際の出力はベアメタル版だけで、ホスト版の `uart` は `--midi-dump` のファイルに送るバイト列を書く。Raspberry Pi 3 では `sc55` は使えない | ○ |
| `midi_sf2` | ファイル名 | なし | SoundFont (SF2) のファイル | ○ |
| `midi_voices` | `4`〜`256` | `48` | SoundFont の音源の同時に鳴らすボイスの数 | ○ |
| `midi_mt32_ctrl`、`midi_mt32_pcm` | ファイル名 | なし | MT-32 / CM-32L の CONTROL と PCM の ROM | ○ |
| `midi_sc55` | ディレクトリ名 | なし | SC-55mkII の ROM を置いたディレクトリ | ○ |
| `midi_uart` | `0`〜`5`、`10` | `0` | `midi_out=uart` で使う UART の番号 (使える番号は機種で違う) | - |
| `midi_uart_family` | `gs`、`mt32` | `gs` | UART の先の音源の種類 | ○ |
| `net` | `on`、`off` | `off` | ネットワークの口 (HTTP と syslog) ([network-api.md](docs/network-api.md)) | - |
| `net_ip`、`net_gw`、`net_dns` | アドレス | DHCP | 固定のアドレス (`192.168.1.50/24`)、ゲートウェイ、DNS サーバ | - |
| `net_name` | ホスト名 | `pix68k` | DHCP で送るホスト名 | - |
| `net_allow` | CIDR かアドレスの並び | なし | ネットワークの口を使ってよい相手 | - |
| `net_token` | 文字列 | なし | 書き込みとファイルの読み出しに要るトークン | - |
| `syslog` | `アドレス:ポート` | なし | ログを UDP の syslog で送る先 (ポートを省くと 514) | - |
| `keyboard` | `auto`、`usb`、`pi400us`、`pi400jp` | `auto` | キーボードの種類。`auto` は VID:PID から決める ([keyboard.md](docs/keyboard.md)) | - |
| `hat` | `auto`、`0`、`1` | `auto` | ゲームパッドのハットの値の範囲を 0-7 / 1-8 に固定する | - |
| `pad_log` | `on`、`off` | `off` | ゲームパッドのボタンとハットの生の値が変わるたびにログに出す (配置の分からないゲームパッドの並びを調べるため。調べ終えたら `off` に戻す) | - |
| `video_scale` | `off`、`integer`、`aspect`、`full` | ベアメタル版 `integer`、ホスト版 `aspect` | 画面の拡大の仕方 (下の「画面の出し方」) | ○ |
| `video_filter` | `scanline`、`curve`、`mask`、`cz-600de`、`cz-600db` を `,` で並べる。`none` | `none` | ブラウン管のように見せる効果と、モニタの枠 (同上) | ○ |
| `osd` | `on`、`fps`、`off` | `on` | 画面の右上に fps、速さ、温度、IP アドレスなどを出す。ベアメタル版は余白が足りなければ `on` でも画面に重ねる。ホスト版は `fps` のときだけ重ねる | ○ |
| `render_core`、`sound_core` | `on`、`off` | `on` | `off` で画面 / 音の合成もコア 1 で行う | - |
| `autotype` | 文字列 | なし | X68000 を動かし始めて `autotype_wait` 秒たったら打つ文字列 (`POST /type` と同じエスケープ。下の「起動の組み合わせ」) | ○ |
| `autotype_wait` | `0`〜`600` | `10` | `autotype` を打つまでの秒数 (ホスト版は 1 秒を 55 フレームと数える) | ○ |
| `profile_wait` | `0`〜`60` | `5` | 起動の組み合わせのメニューで待つ秒数。`0` ならメニューを出さずに前回のものにする (`pix68k.cfg` に書く) | - |

キーの表は `src/platform/settings.h` の `kSettingKeys` にあり、ベアメタル版、ホスト版、設定メニューが同じ表を使う。

#### 画面の出し方 (`video_scale=`、`video_filter=`)

`video_scale=` で X68000 の画面の拡大の仕方を、`video_filter=` でブラウン管のモニタのように見せる効果を決める。2 つは自由に組み合わせられる (Raspberry Pi 3 は下の制限がある)。

| `video_scale=` | 拡大の仕方 |
|---|---|
| `off` | 拡大しない (1 倍で真ん中に出す) |
| `integer` | 整数倍 (ベアメタル版の既定)。768x512 の枠の 1 倍か 2 倍に入る最大の倍率 (1920x1080 なら、どの画面モードも 1536x1024 くらい) |
| `aspect` | 表示の高さいっぱいで、横は X68000 のモニタと同じ 4:3 (1920x1080 なら 1440x1080)。どの画面モードも同じ枠 (ホスト版の既定) |
| `full` | 表示いっぱいに引き伸ばす (1920x1080 なら 1920x1080) |

| `video_filter=` に書く名前 | 掛けるもの |
|---|---|
| `scanline` | スキャンライン (元の 1 行から作る表示の行のうち、最後の 1 行を半分の明るさにする。縦 2 倍以上に拡大したときだけ) |
| `curve` | たる型のゆがみ (四隅が内側に寄り、外は黒) と、四隅を暗くする |
| `mask` | アパーチャーグリル (横 3 画素ごとの R、G、B の縦じま) と、横の少しのにじみ |
| `cz-600de` | モニタの枠 (グレー)。下の「モニタの枠」 |
| `cz-600db` | モニタの枠 (黒) |

`video_filter=scanline,curve,mask` のように `,` で並べる (順番は問わない)。`none` か空なら効果なし (既定)。知らない名前は飛ばして、知っている名前だけを使い、ログに `video_filter=scanline,foo: unknown names ignored` のように出す (後の版で名前が増えても、そのファイルを前の版で読めるように)。
X68000 の画面の中身は変えないので、`/screen.ppm`、`--screenshot`、ステートには入らない。右上の数字 (`osd=`) と左上のメッセージには掛けない。右上の数字と左上のメッセージは、`video_scale=` の枠の外の余白に置く (余白が足りないとき、ベアメタル版は X68000 の画面に重ねる。`full` には余白がない)。

`aspect` と `full` の高さは、ベアメタル版では 1200 ラインまで (3840x2160 では `aspect` が 1600x1200、`full` が 2133x1200)。枠が元の画面より小さくなるとき (`pix68k_gpu_scale=2` でフレームバッファが 960x540 のときの 768 ドットの画面など) と、`pix68k_gpu_scale` の縦と横の倍率が違うときは、整数倍にする。
整数倍でない枠では横の倍率も整数にならない (768 ドットを 1440 にすると 1.875 倍) ので、`mask` (にじみ) がなければ、元の 1 ドットが表示の 1 ドットと 2 ドットに分かれる。スキャンラインは整数でない倍率でも暗い行が元の 1 行に 1 行だけなので、縞 (モアレ) は出ない。

ホスト版も同じ枠でウィンドウの中に出す。既定は `aspect` で、ウィンドウに入る最大の 4:3 にする (既定の 768x512 のウィンドウなら 682x512)。`aspect` と `full` は、ウィンドウが X68000 の画面より小さくても縮めて出し、切らない (`integer` と `off` は、ベアメタル版と同じく真ん中を切り出す)。前の版のホスト版は、ウィンドウに入る最大の 3:2 に引き伸ばしていた (768x512 のウィンドウなら 768x512)。ウィンドウいっぱいに出すなら `full` にする。

設定メニューでは Display のページ (Scale、Scanlines、Curve、Mask) で切り替え、すぐに見た目が変わる。主のページには `aspect + scanline,curve` のようにまとめて出す。
Raspberry Pi 3 (Zero 2 W、3A+ も同じ) では、`curve` と、`full` と `mask` の組は選べない (CPU で作るので、1 フレームの時間に収まらない)。`pix68k.cfg` に書いてあれば、`curve`、`mask` の順に外して選べる組み合わせにし、ログに出す。Raspberry Pi 4 / 5 ではどれも選べる。
作り方と、かかる時間は [baremetal.md](docs/baremetal.md) の「画面の出し方」を参照。

##### モニタの枠 (`cz-600de`、`cz-600db`)

`video_filter=` に `cz-600de` (グレー) か `cz-600db` (黒) を足すと、画面の周りにブラウン管のモニタの枠の絵を出し、X68000 の画面をそのガラスの部分 (1920x1080 の中の 1160x870、4:3) に写す。ベアメタル版だけで、ホスト版はログに `video_filter=cz-600de: the bezel is not supported by the host version; ignored` と出して枠なしで出す (設定は消さない)。

- 枠の絵は起動したドライブ (SD カード、NVMe、USB) の `/pix68k/bezel-cz-600de.bin` と `bezel-cz-600db.bin`。配る zip の `pix68k/` に入っている (自分で作るなら `tools/bezel_cz600d.py` で PNG を描き、`tools/bezel_pack.py` で .bin にする。どちらも Python 3、numpy、Pillow が要る)。
- 2 つは同時に使えない。両方書くと、あとに書いたほうを使い、ログに `video_filter=cz-600de,cz-600db: only one bezel can be used; using the last one` と出す。
- 枠を使うと、`video_scale=` の値によらずガラスの部分に写す。`scanline`、`curve`、`mask` は、ガラスの中の絵に掛かる。`curve` がなくても枠は出せる。
- 枠を出すのは、フレームバッファが 1920x1080 で、X68000 の画面がガラスの部分 (1160x870) に入るときだけ。ほかの解像度に合わせて縮めることはしない。次のときは枠なしで `video_scale=` のとおりに写し、ログに理由を出す: 絵のファイルがない (`not found`)、壊れている、フレームバッファが 1920x1080 でない (`is 1920x1080 but the screen is ...`)、`pix68k_gpu_scale` で GPU に拡大させている (フレームバッファが 1920x1080 より小さくなるので、ふつうは大きさが違うというログになる)、画面がガラスより大きい (768x1024 など。`does not fit in the glass`)。
- 右上の数字と左上のメッセージは、枠の外の暗い背景に出し、背景は枠の絵を半分の明るさにしたものにする (黒の半透明に見える)。
- Raspberry Pi 3 でも使える。ただし、選べる組み合わせは枠がないときと同じ (`curve` と、`full` + `mask` は選べない)。枠を出せないときは `video_scale=` のとおりに写すため。
- 設定メニューでは Display のページの Bezel (off、cz-600de、cz-600db) で切り替える。初めて選んだときに絵を起動したドライブから読む (8.3MB。Pi 5 で 0.5 秒ほどかかる)。絵が読めなければ選べない。設定に枠を書いてあって絵が読めないときも、枠なしで写しているので、Scale や効果は今までどおり変えられる。

公開版 v0.1.0 の `scale=` と `scanline=` も読み、v0.1.0 と同じ見た目になるように読み替える。ログに `scale=off is old; use video_scale= (Save settings rewrites it)` のように出す。設定メニューの Save settings で保存すると、その行を消して `video_scale=` と `video_filter=` の行を書く (何も変えていなくても)。

| v0.1.0 の書き方 | 読み替え |
|---|---|
| `scale=auto` | `video_scale=integer` |
| `scale=off` | `video_scale=off` |
| `scanline=on` | `video_filter=scanline` |
| `scanline=off` | `video_filter=none` |

- 読み替えは、ファイルを読み終えてから (起動の組み合わせなら `pix68k.cfg` と重ねて読んだあとで) 行う。同じキーどうしは、あとの行が勝つ。`scale=` と `video_scale=`、`scanline=` と `video_filter=` も同じで、あとにある行の値を使う (重ねて読むときは、組み合わせのファイルの行があと)。
- ホスト版は、v0.1.0 のホスト版と同じく `scale=` と `scanline=` を読まない (知らせず、保存でも書き直さない)。ベアメタル版と同じ `pix68k.cfg` を使っても、ベアメタル版の見た目は変わらない。
- 開発中の版にあった `crt=` は読み替えない。ログに `crt=full is no longer supported; use video_filter= and video_scale= (ignored)` と出して無視する (行は残すので、`video_filter=` と `video_scale=` に書き直す。`grille` は `scanline,mask`、`geom` は `video_scale=aspect` と `scanline,curve`、`full` は `video_scale=aspect` と `scanline,curve,mask`)。

### 起動の組み合わせ (pix68k-<名前>.cfg)

ゲームごとの構成 (OutRun、グラディウス II、Human68k など) を `/pix68k/pix68k-<名前>.cfg` に分けて置くと、起動のたびに選べる。
名前は英数字と `-`、`_` だけ (32 字まで)。書き方は `pix68k.cfg` と同じで、1 行目が `#` で始まれば、その残りをメニューに出す題にする。

```
# グラディウス II
fd0 = GRADIUS2.XDF
clock = 10
autotype = gr2\n
autotype_wait = 15
```

- **起動時のメニュー**: 組み合わせが 1 つでもあれば、起動したときに HDMI に一覧を出す。先頭は `pix68k.cfg` (組み合わせなし)。
  ↑↓ (ゲームパッドは上下) で選び、Enter (ゲームパッドは A / B) で決める。`profile_wait=` 秒 (既定 5) 何も押さなければ、前回選んだものになる。
  何か押すと待つのをやめる (Shift などの修飾キーだけと、ゲームパッドの左右では止まらない。起動したときから押したままのゲームパッドのボタンも数えない)。組み合わせが 1 つもなければメニューは出さず、今までどおり `pix68k.cfg` で起動する。
  メニューを出すときは、その分 (`profile_wait=` 秒ほど) 起動が遅くなる。キーボードとゲームパッドのために USB の装置を X68000 を動かす前に探し、探し終えてから少なくとも 2 秒は待つ
- **pix68k.cfg との重ね方**: 組み合わせを選んだときは、`pix68k.cfg` からは共通の項目 (`net`、`net_*`、`syslog`、`keyboard`、`hat`、
  `video_filter`、`video_scale`、`osd`、`render_core`、`sound_core`、`midi_uart`、`midi_uart_family`、`write_back`、`profile_wait`。前の `scale`、`scanline`、`crt` も) だけを受け継ぎ、
  X68000 の構成 (CPU、メモリ、ディスク、拡張ボード、MIDI、`autotype`) は組み合わせのファイルだけから読む。書いていない項目は既定の値になる。
  `pix68k.cfg` の `fd0=` などが組み合わせに混ざらないようにするためで、`pix68k.cfg` に写して使っていた控えのファイルは、そのまま置けば写したときと同じ構成になる。
  組み合わせのファイルに共通の項目を書けば、そちらを使う
- **前回の選択**は `/pix68k/profile.sel` に名前を 1 行で書いて覚える。前と違うものを選んだときと、HTTP で頼まれたときだけ書く
- **autotype**: `autotype=` の文字列を、X68000 を動かし始めて `autotype_wait=` 秒 (既定 10) たったら打つ。書き方は HTTP の `POST /type` と同じ
  (`\n` は Enter、`\e` は ESC。`#` は注釈になるので書けない)。`pix68k.cfg` に書けば、組み合わせなしで起動したときに打つ
- **HTTP**: `POST /reboot?profile=<名前>` で、`pix68k.cfg` を書き換えずにその組み合わせで再起動する (メニューは出さない)。[network-api.md](docs/network-api.md)
- **設定メニューの「Save settings」**は、組み合わせで動いているときは、共通の項目を `pix68k.cfg` に、そのほかを組み合わせのファイルに書く (共通の項目でも、組み合わせのファイルにその行があれば組み合わせのファイルに書く)
- ホスト版は `--profile <名前>` で、`--config` のファイルと同じディレクトリの `pix68k-<名前>.cfg` を同じように重ねて読む (メニューはない)

### cmdline.txt (ベアメタル版)

SD カードのルートの `cmdline.txt` に書く。主なもの:

- `pix68k_sound=pwm` / `i2s` / `off`: 音の出し先 (既定は HDMI)。I2S の DAC のつなぎ方は [baremetal.md](docs/baremetal.md) の「I2S の DAC」
- `pix68k_fan=auto` / `full` / `off`: Raspberry Pi 5 のファン
- `pix68k_drive=auto` / `usb` / `sd` / `nvme`: `/pix68k` を読むドライブ
- `width=1920 height=1080`: フレームバッファの大きさ (Circle のオプション)
- `pix68k_net=on` など: `pix68k.cfg` のネットワークのキーに `pix68k_` を付けて書くと、こちらを使う

全部の一覧は [config-reference.md](docs/config-reference.md) にある。

### ホスト版のオプション

| オプション | 内容 |
| --- | --- |
| `-r DIR` | IPL ROM と CGROM の場所 (既定 `~/.cache/pix68k/rom`) |
| `-0 FILE`、`-1 FILE` | FD0 / FD1 のイメージ |
| `-h FILE` | SASI ID 0 のハードディスクのイメージ |
| `--sc ID FILE`、`--cd FILE` | SCSI のハードディスク、CD-ROM のイメージ |
| `--cpu 68000\|68030\|68060`、`--clock MHz`、`--ram MB` | 機種、クロック、本体のメモリ |
| `--ipl FILE` | IPL ROM のファイル |
| `--config FILE` | 設定ファイル (`pix68k.cfg` と同じ形式)。コマンドラインのほうが優先する |
| `--profile NAME` | `--config` と同じディレクトリの `pix68k-NAME.cfg` を重ねて読む (上の「起動の組み合わせ」) |
| `--write-back`、`--read-only` | 終了時にディスクのイメージに書き戻す / 書き込み禁止にする (既定はどちらもしない) |
| `--max-speed` | 最高速モード |
| `--no-audio`、`--wav FILE` | 音を出さない / WAV に書く |
| `--joy-keys` | カーソルキーと Z / X をジョイスティック 1 にする |
| `--time N` | RTC の時刻を決める (前の版と画面や音を比べるとき) |

全部の一覧は `host/main.cpp` の先頭の注釈にある。

## 操作

- 設定メニュー: Scroll Lock、Application (メニューのキー)、Menu のどれかで開く / 閉じる。Raspberry Pi 400 はラズベリーのキー + M、ホスト版は F12 でも開く。
  ゲームパッドでは Home か、Select を押しながら上で開く / 閉じる (ベアメタル版では、Circle が配置を知っているゲームパッドか、pix68k の表にあるゲームパッド)。中では十字キーで選び、A で決め、B で戻る / 閉じる。
  FD の入れ替え、CD-ROM の入れ替え、機種と拡張ボードの選択、ステートの保存と読み込み、リセット、設定の保存ができる。
  開いている間は X68000 を止める ([settings-menu.md](docs/settings-menu.md))
- Ctrl + Alt + Delete: 書き戻していないディスクと SRAM を書いてから Raspberry Pi を再起動する。
  X68000 のリセット (CTRL + OPT.1 + DEL) は Ctrl + GUI (Windows / Command のキー) + Delete で打つ ([keyboard.md](docs/keyboard.md))
- キーボード: ホストのキーを、JIS 配列で同じ位置の X68000 のキーにする。XF1〜XF5、かな、OPT.1 / OPT.2 などの割り当ては [keyboard.md](docs/keyboard.md) の表にある。
  USB キーボードは 4 台までいっしょに使える
- Raspberry Pi 400: 本体のキーボードは、ラズベリーのキーを押している間ほかのキーを X68000 にしかないキーに置き換える (ラズベリー + 1〜5 が XF1〜XF5 など)
- マウスとゲームパッド: USB マウスは X68000 のマウス、USB ゲームパッドはジョイスティック 1、2 になる。
  ゲームパッドの Start と Select を同時に押すと、X68000 をリセットする (設定メニューの Reset と同じ。ベアメタル版では、Circle が配置を知っているゲームパッドか、pix68k の表にあるゲームパッド (今は HORIPAD S だけ) で使える)
  ホスト版では、ウィンドウの中を左クリックするとマウスを取り込み、中ボタンのクリックか Ctrl + Alt で戻す ([build-and-test.md](docs/build-and-test.md))

## ネットワークの口

ベアメタル版で `net=on` にすると、Raspberry Pi の有線 LAN で HTTP (80 番) のサーバと syslog の送信を動かす。
SD カードを抜かずに、カーネルや設定の入れ替え、再起動、ログと画面の取得、キー入力、画面へのメッセージの表示ができる。Wi-Fi は使えない。

```sh
curl http://pix68k/log                                  # ログ
curl -o screen.ppm http://pix68k/screen.ppm             # 画面
curl -H 'X-Pix68k-Token: 好きな文字列' -T kernel8.img http://pix68k/files/kernel8.img
curl -H 'X-Pix68k-Token: 好きな文字列' -X POST http://pix68k/reboot
```

`neptune=on` で `neptune_link=eth` (ベアメタル版の既定) のときは、同じ Ethernet を Neptune-X が使うので、`net=on` でも HTTP と syslog は動かない ([expansion.md](docs/expansion.md))。
書き込みには `net_token=` か `net_allow=` の設定が要る。口の一覧と決まりは [network-api.md](docs/network-api.md) にある。
操作には curl のほか、コマンド [pix68kcli](tools/pix68kcli/README.md) (宛先と token を設定ファイルに書いておける) が使える。

## ソースからビルドする

リリースの zip を使うなら、この節は要らない。カーネルを自分でビルドするときや、開発に加わるときに使う。

### ベアメタル版

```sh
git submodule update --init
brew install aarch64-elf-gcc qemu mtools
tools/build_baremetal.sh 3          # Raspberry Pi 3     → build/baremetal-raspi3/app/kernel8.img
tools/build_baremetal.sh 4          # Raspberry Pi 4 / 400 → build/baremetal-raspi4/app/kernel8-rpi4.img
tools/build_baremetal.sh 5          # Raspberry Pi 5     → build/baremetal-raspi5/app/kernel_2712.img
tools/build_baremetal.sh 3 --qemu   # QEMU 用の Raspberry Pi 3 版。tools/qemu_smoke.sh で起動を確かめる
tools/make_release.sh v1.0          # 3 つをビルドし、配る zip を build/release/pix68k-v1.0.zip に作る (試しなら --allow-dirty)
```

Linux (Ubuntu) では `sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-system-arm mtools rsync zip unzip curl` を入れ、
`PREFIX64=aarch64-linux-gnu- tools/build_baremetal.sh 3` のように `PREFIX64` を付けてビルドできる。
QEMU での試験、カーネルの大きさの上限などは [baremetal.md](docs/baremetal.md) にある。

できたカーネルを SD カードのルートに置き換えれば動く。SD カードを一から作るときは、`tools/make_release.sh` の zip を使うか、
次の手順で作る ([baremetal.md](docs/baremetal.md) の「SD カードの用意」)。

1. Raspberry Pi のファームウェア (Circle の `boot/` で `make` を実行するとダウンロードする) をルートに置く
2. `config.txt` をルートに置く。Circle の `boot/config64.txt` をもとにし、`[pi4]` の節の `armstub=armstub8-rpi4.bin` の行を消し、
   `[pi5]` の節に `framebuffer_depth=32` と `framebuffer_ignore_alpha=1` を書く (ないと Raspberry Pi 5 では何も出ずに止まる)
3. カーネル (`kernel8.img`、`kernel8-rpi4.img`、`kernel_2712.img`) をルートに置く

公開リポジトリ (j3tm0t0/pix68k) に `v` で始まるタグを push すると、`.github/workflows/release.yml` が GitHub のホストの runner で
`tools/make_release.sh` の zip と、`tools/pix68kcli/build.sh` の pix68kcli の 7 つを作り、SHA256SUMS と一緒に Releases に載せる。
説明の本文は `docs/releases/<タグ>.md` に書いておく ([docs/releases/README.md](docs/releases/README.md))。
タグを打たずに作るところまでを試すときは、Actions の画面か `gh workflow run release.yml --ref <ブランチ>` で動かす (Release は作らず、artifact に置く)。
`-f notes_tag=v0.2.0` を付けると、説明の本文も `docs/releases/v0.2.0.md` で作る。

### ビルドとテスト (ホスト)

ホスト版には SDL2 が要る (macOS は `brew install sdl2`)。

```sh
cmake -S . -B build
cmake --build build -j
./build/pix68k -0 disk.xdf                  # X68000 (16MHz) で FD0 から起動する
./build/pix68k --cpu 68030 -0 disk.xdf      # X68030
./build/pix68k --cpu 68060 -0 human060.xdf  # 060turbo (060turbo.sys を組み込んだ起動ディスクで)
./build/pix68k --config ~/x68k/pix68k.cfg   # ベアメタル版と同じ形式の設定ファイルを読む
(cd build && ctest)                         # 試験 (68000 の SingleStepTests は tools/fetch_sst.sh で先に取得する)
```

Linux (Ubuntu) では `sudo apt install build-essential cmake ninja-build libsdl2-dev python3` を入れると同じ手順でビルドできる。
ctest、速さの測り方 (`bench`、`machine_bench`)、CI は [build-and-test.md](docs/build-and-test.md) にある。

### RetroArch / libretro (開発中)

現在は 68000 / メモリ 2MB / 標準音源 (FPU ボードなし) で XDF / DIM / D88 の FD と SASI の HDF から起動できる。
ROM は RetroArch の System ディレクトリ内の `pix68k/IPLROM.DAT` (128KB) と `pix68k/CGROM.DAT` (768KB) に置く
(小文字のファイル名も使える)。コンテンツなしでコアを起動すると、IPL ROM だけで起動する。

```sh
cmake -S . -B build/libretro -DPIX68K_HOST=OFF -DPIX68K_LIBRETRO=ON
cmake --build build/libretro --target pix68k_libretro libretro_api_test libretro_runtime_test -j
ctest --test-dir build/libretro -R '^libretro_' --output-on-failure
```

SDL2 は不要。出力は macOS では `pix68k_libretro.dylib`、Linux では `pix68k_libretro.so`。
`PIX68K_HOST=ON` と併用すれば SDL2 版も作れる。RetroArch の「コアをロード」から読み込むか、
`retroarch -L build/libretro/pix68k_libretro.so disk.xdf` で起動する (macOS では `.dylib`)。
一緒にできる `pix68k_libretro.info` を RetroArch の「コア情報」のディレクトリに置くと、名前と必要な ROM が表示される。
Core Options で 2MB / 12MB、16MHz / 10MHz、RTC の開始時刻を選ぶ。変更はコンテンツの再ロードで反映する。
RTC の `fixed` は地方時 2023-11-14 22:13:20 (1700000000 秒) から始める。既定の `host` は現在の地方時。

キーボードは PC のキーに相当する USB HID に変換する。F11 / F12 は XF1 / XF2、Alt は XF1 / XF5、
Windows / Command は OPT.1 / OPT.2。ゲームパッドの十字キーと B / A は ATARI の方向とトリガ A / B になり、
2 ポート使える。マウスは相対移動と左右ボタンを渡す。SRAM は RetroArch の SAVE_RAM として保存・復元する。
Core Options のパッド補助を有効にすると、L＋ボタンでキー入力、右スティックでマウス操作ができる。
通常版は既定で無効、Vita版は有効。[操作表](libretro/vita/README.md#パッドでキーマウスを操作) を参照。

複数枚の FD は `.m3u` に 1 行 1 ファイルで書く (プレイリストからの相対パスも使える。最大 64 枚)。
先頭 2 枚を FD0 / FD1 に入れる。交換するときは Core Options の `Disk Control target drive` で FD0 / FD1 を選び、
RetroArch のディスク操作で「取り出す → イメージを選ぶ → 挿入する」の順に操作する。
HDF は SASI ID 0 に接続する。HDF と M3U 内の FD の同時指定はまだできない。

ディスクは既定で書込み禁止。`Disk writes` を `save-copy` にしてコンテンツを読み直すと、
RetroArch の保存先内の `pix68k-media` に変更したイメージを保存する。元イメージは読み取るだけで変更しない。
ファイル名は元パスと元データから決めるので、同名の別ゲームや元データを交換したものと保存が混ざらない。
同じ元パス・データで次に起動すると保存コピーを使う。コピーの拡張子は `.pxm` とし、ゲーム一覧への混入を避ける。最後の書込みから約 2 秒間変更がない媒体を保存し、排出時・終了時にも保存する。
保存時は全イメージを同期して書き出すため、大きい HDF や遅い保存先では一時停止が生じる場合がある。
保存失敗は通知し、実行中は変更を保持して再試行する。FD は各 8MB / 合計 128MB、HDF は 256MB まで。
ステートは RetroArch の保存・復元から使える。CPU・メモリ・周辺機器に加え、画面バッファ、音の補間位置と未送信音声、
押下中のキー、FD0 / FD1 の挿入状態を保存する。RAM の使用量が増えても必要なバッファ容量は増えない。
ディスクの中身はステートに含めない。一覧や内容が保存時と異なる場合は、現在の実行状態を変えずに復元を断る。
設定 (RAM / クロック) や ROM が異なる場合も同様。RetroArch 用ステートは、表示・入力の情報を加えたラッパー形式。
巻戻し・runahead・netplay の実用性はまだ検証していない。
画面の解像度と周期は CRTC の設定に合わせ、音は 48kHz のステレオで出す。
共有ライブラリを実際にロードする API 試験、合成 ROM の画面モード変更・音・入力の比較試験は ROM なしで動く。
IPL ROM / CGROM と Human68k の起動ディスク (`~/.cache/pix68k`) があれば、2MB / 16MHz と 12MB / 10MHz で
各 1000 フレーム動かして `DIR` を打ち、直接動かした共通コアと画面・音・RAM が一致するかも調べる。
ROM とディスクは検証用ディレクトリにコピーして使い、元ファイルは変えない。
RetroPie 用の独立モジュールと導入手順は [retropie/README.md](retropie/README.md)。
64bit Linux が対象。Vita 向けの静的ビルドは [libretro/vita/README.md](libretro/vita/README.md) (開発中)。

## 資料

詳しい説明は `docs/` にある。文中の #番号 は、開発用の非公開リポジトリの issue と PR の番号。

| ファイル | 内容 |
| --- | --- |
| [build-and-test.md](docs/build-and-test.md) | ホスト版のビルド、ctest と速さの測り方、音と RTC のオプション、ホスト版の入力、CI |
| [baremetal.md](docs/baremetal.md) | ベアメタル版のビルド、config.txt、コアの割り当て、音、ファン、ログ、画面の拡大と OSD、I2S の DAC、SD カードと USB メモリ、起動にかかる時間 |
| [config-reference.md](docs/config-reference.md) | SD カードに置くファイル、pix68k.cfg のキー、cmdline.txt のオプションの詳しい説明 |
| [settings-menu.md](docs/settings-menu.md) | 設定メニューの項目、ステートの保存と読み込み |
| [keyboard.md](docs/keyboard.md) | USB キーボード、Ctrl + Alt + Delete、Raspberry Pi 400 のキーボードのキーの置き換え |
| [network-api.md](docs/network-api.md) | ネットワークの口 (HTTP と syslog) |
| [x68030.md](docs/x68030.md) | X68030 (68030 と 68882) |
| [060turbo.md](docs/060turbo.md) | 060turbo (68060)、68060 のサイクル数、本体のバス、最高速モード |
| [disks-and-scsi.md](docs/disks-and-scsi.md) | SASI の HD のイメージの作り方、増設メモリ、外付け SCSI ボード、CD-ROM、ディスクの書き戻し |
| [expansion.md](docs/expansion.md) | 数値演算プロセッサボード、まーきゅりーゆにっと、Neptune-X、RS-232C |
| [midi.md](docs/midi.md) | MIDI ボード CZ-6BM1 と音源 (SoundFont、Munt、Nuked-SC55、UART)。試験的な機能 |
| [video.md](docs/video.md) | 画面の合成の決まり |
| [compat.md](docs/compat.md) | ゲームとソフトの動作の記録 |
| [perf-060-astra.md](docs/perf-060-astra.md) | 68060 のコアのホストでの計測の記録 |
| [tools/pico_kbd/README.md](tools/pico_kbd/README.md) | 実機の試験に使う Pico のファームウェア (USB キーボード、UART の中継) |

### ゲームとソフトの動作の記録

市販のゲーム (グラディウス、悪魔城ドラキュラなど)、同人ゲーム、ベンチマーク、ドライバを動かした結果は
[docs/compat.md](docs/compat.md) にまとめている。機種 / CPU、ホスト版か Raspberry Pi 3 / 4 / 5 か、確かめた PR、結果を 1 行ずつ書く。
確かめたら、そこに行を足す。

## ほかのエミュレータとの関係

pix68k のソースには、ほかの X68000 エミュレータのソースコードは入っていない。

- XEiJ (https://stdkmd.net/xeij/) は、次の使い方をしている。
  - XEiJ の説明書 (HTML) を、仕様の資料の 1 つとして読んだ。
  - XEiJ に付いている試験プログラム (`misc/instructiontest.x`、`misc/fputest.x`) を pix68k の上で動かし、出力を比べる相手にした。
    試験プログラムはリポジトリに入れておらず、各自の XEiJ のものを使う。
  - XEiJ に付いている `IPLROM30.DAT` と、XEiJ が作る IPLROM 1.6 は、それぞれ X68030 と 060turbo の IPL ROM (起動用の ROM) として使える。
    これらの ROM は、リポジトリにも配布物にも入れない。
- 開発の初めに、XEiJ のソースを読んだうえで書いた部分があった。その部分は、XEiJ のソースを見ずに、
  データシート、SCSI-2 などの規格、Inside X68000 などの書籍、XEiJ の説明書、公開されている資料 (X68k:IOMAP、x68000-dev-guide など)、IPL ROM の逆アセンブル、ソフトの動き、試験プログラムの結果を根拠に書き直した。資料で決まらない細かい所は、書き直す前の版と同じ場面を動かして出力を比べ、同じになるように決めた所がある (その所では、書き直す前の版のソースは見ていない)。x68000-dev-guide の一部は MAME のソースを出どころにした第三者の資料で、MAME のソースそのものは読んでいない。
  公開リポジトリは書き直したあとのコードから作り、それより前の変更の履歴は入れていない。
- 68000 の CPU コアは、SingleStepTests (680x0。命令ごとの試験データ) の結果と合わない所で、
  WinUAE (Amiga のエミュレータ) の動作か実機の計測値に合わせている。試験ごとの理由は `tests/sst/expected_failures.txt` に、分け方の規則は `tools/classify_sst.py` にある。

## 謝辞

pix68k は、次のソフトウェア、資料、計測の結果を使って作った。作者のみなさんに感謝する。

ソースを取り込んだり、リンクしたりして使っているもの (ライセンスは下の「ライセンス」と `NOTICE`):

- [Circle](https://github.com/rsta2/circle) (rsta2): ベアメタル版の起動、USB、HDMI、SD カード、割り込み、TCP/IP と、addon の FatFs でのファイルの読み書き。ベアメタル版はこの上で動く。設定メニューの文字は、Circle の 8x16 のフォント (元は Linux の kbd の lat1-16) から作った
- [ymfm](https://github.com/aaronsgiles/ymfm) (Aaron Giles): OPM (YM2151) のエミュレーションのもとにした
- [Munt](https://github.com/munt/munt) の libmt32emu: MIDI ボードの先の MT-32 / CM-32L の音源
- [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) (nukeykt): MIDI ボードの先の SC-55mkII の音源
- [TinySoundFont](https://github.com/schellingb/TinySoundFont) (Bernhard Schelling) と、そのもとの [SFZero](https://github.com/stevefolta/SFZero) (Steve Folta): SoundFont の音源

試験に使っているもの (リポジトリには入れず、必要なときに取得するか、各自のものを使う):

- [SingleStepTests 680x0](https://github.com/SingleStepTests/680x0): 68000 のコアを命令ごとに確かめる試験データ (CLK で作られたもの)。`tools/fetch_sst.sh` で取得する
- [Musashi](https://github.com/kstenerud/Musashi): 68030 のコアと 1 命令ずつ結果を比べる相手。`tools/fetch_musashi.sh` で取得する
- [XEiJ](https://stdkmd.net/xeij/): 説明書 (HTML) を仕様の資料の 1 つにした。付いている試験プログラム `misc/instructiontest.x` と `misc/fputest.x` の出力を、68030 と FPU を確かめる相手にした。
  付いている `IPLROM30.DAT` と、XEiJ が作る IPLROM 1.6 は、X68030 と 060turbo の IPL ROM として使える (どちらもリポジトリには入れない)
- WinUAE (Amiga のエミュレータ): 68000 のコアが SingleStepTests と食い違う所で、動作を合わせる相手にした (コードは入れていない)
- 動作を確かめるのに使った X68000 のソフト: si.r ([kg68k/si-ee](https://github.com/kg68k/si-ee))、TOMY さんの cpupower.r、GORRY さんの susie.x、
  Tany さんの PCM8PP、Shi-MAD さんの ether_ne.sys、計測技研の TCPPACKA、ArctanX さんの MDX の曲など。ほかは [docs/compat.md](docs/compat.md) にある
- 060turbo.sys (0.56、0.61): 逆アセンブルして、組み込んだときの IOCS の書き換えやオプションの扱いを確かめた
- mdxtools: MDX の曲の再生のテンポを比べる相手にした

仕様や値の根拠にした資料:

- 「Inside X68000」(桑野雅彦、ソフトバンク、1992 年): 画面の合成、SASI と SCSI、数値演算プロセッサボードなど
- Data Crystal の X68k:IOMAP: I/O の番地とレジスタのビット
- x68000-dev-guide (JC-000) と、chibiakumas の X68000 の解説: パレットとグラフィックのビットの並び
- Seafy さんの「CRTC の設定 その他」(1993 年): 色 $0000 のドットを透明にする決まり
- yunkya2 さんの wifipcm (GIMONS さんの PilederX をもとにした互換ボード) の README と pcm.c の説明: まーきゅりーゆにっとのレジスタ
- Makoto Kamada さんと GIMONS さんの投稿 (2015 年。posfie「X68000 の挙動いろいろ」のまとめ): MFP、ADPCM、スプライトを読み書きするときのウェイト
- Cwik さんの計測値: 68000 の DIVS がオーバーフローしたときのサイクル数 (`tools/classify_sst.py`)
- Motorola の M68060 User's Manual、MC68030 User's Manual、MC68881/MC68882 User's Manual: 命令の動作とサイクル数
- 各 LSI のデータシート (YM2151、MSM6258、MC68450、MB89352、Z8530、DP8390D など): 周辺機器のレジスタと動作
- NetBSD/x68k の Neptune-X のドライバ (`neptune.c`) と起動プログラム (`stand/boot` の `ne.c`、`dp8390.c`): Neptune-X のレジスタの使われ方 (コードは写していない)
- [えくしび「060turboレプリカ」](https://xbeeing.com/060turbo-replica-x68030/) と [X68030 / 040turbo / 060turbo のキャッシュとパフォーマンス](http://tomoretropc.blogspot.com/2020/06/x68030-040turbo-060turbo.html): 実機の X68030 と 060turbo の si.r の値
- mt32-pi: MIDI ボードの出力を UART でつなぐときの、mt32-pi の側の設定の例 (mt32-pi のソースから読んだもの)
- [T10 の SCSI の操作コードの一覧](https://www.t10.org/lists/op-num.htm): SCSI のコマンドの名前
- [pi400kb](https://github.com/Gadgetoid/pi400kb): Raspberry Pi 400 のキーボードの VID:PID
- Raspberry Pi の Linux の device tree ([bcm2712-rpi-5-b.dts](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts)): Raspberry Pi 5 のファンのつなぎ方と回す速さ

## ライセンス

GPLv3 (Circle が GPLv3 のため)。`LICENSE` を参照。

`src/x68k/opm.{h,cpp}` (YM2151) は [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause、Copyright (c) 2021, Aaron Giles) をもとにしている。
ymfm の著作権表示、条件、免責の全文は `src/x68k/opm.h` と `src/x68k/opm.cpp` の先頭と、`NOTICE` にある (バイナリを配るときは `NOTICE` を一緒に置く)。

`external/tinysoundfont/tsf.h` は [TinySoundFont](https://github.com/schellingb/TinySoundFont) (MIT、Copyright (C) 2017-2023 Bernhard Schelling、SFZero (Copyright (C) 2012 Steve Folta) をもとにしたもの) を変えずに置いたもの。条件の全文は `external/tinysoundfont/LICENSE` と `NOTICE` にある。

`external/munt/mt32emu` は [Munt](https://github.com/munt/munt) の libmt32emu 2.8.6 (コミット 91d2205e) を変えずに置いたもの
(使うファイルだけ。`external/munt/config/config.h` は CMake が作るヘッダの代わりに書いたもの)。ライセンスは LGPL 2.1 以降
(Copyright (C) 2003-2009 Dean Beeler, Jerome Fisher、Copyright (C) 2011-2026 Dean Beeler, Jerome Fisher, Sergey V. Mikayev)。
条件の全文は `external/munt/mt32emu/COPYING.LESSER.txt` にある。libmt32emu は、ベアメタル版の `kernel8.img` にも、
ホスト版の `pix68k` にも静的にリンクしている。どちらかのバイナリを配るときは、LGPL と GPLv3 の求めに従い、次のどちらかをする。

- 配るバイナリを作ったのと同じ版の pix68k のソース一式 (`external/munt` と `external/circle` を含む) を一緒に渡す
- そのソース一式を手に入れる方法 (リポジトリの URL と、バイナリを作ったコミット) を書いて渡す

ソース一式には、Munt を差し替えてリンクし直す手順も含まれる: `external/munt/mt32emu` を別の版の libmt32emu (または
手を加えたもの) に替え、ホスト版は README の「ビルドとテスト (ホスト)」の手順、ベアメタル版は `tools/build_baremetal.sh`
でビルドし直す。`NOTICE` と `external/munt/mt32emu/COPYING.LESSER.txt` も一緒に置く。

`external/nuked-sc55` は [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) (コミット cc801b26) から、マイコンと PCM の LSI の
エミュレーションのファイルだけを写したもの (SDL の画面と音、MIDI の入力、`main()` は入れていない)。ライセンスは GPL 2 以降
(Copyright (C) 2021, 2024 nukeykt) で、pix68k の GPLv3 と一緒に配れる。全文は `external/nuked-sc55/LICENSE` にある。
pix68k で使うために `src/mcu.cpp`、`src/pcm.cpp`、`src/pcm.h`、`src/submcu.cpp` を変えた (SDL などを除き、大きな配列を確保した
領域へのポインタに変え、グローバル変数を初めの値に戻す関数を足した。変えたところは各ファイルの先頭に書いた)。
