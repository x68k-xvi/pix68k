# pix68kcli

ベアメタル版の HTTP の口 ([docs/network-api.md](../../docs/network-api.md)) を操作するコマンド。curl で token のヘッダを毎回書く代わりに使う。Go の標準ライブラリだけで書いてある。

## ビルド

```sh
cd tools/pix68kcli
go build                 # 手元の環境向け (./pix68kcli)
./build.sh               # 全部の対象を dist/ に作る (CGO なし)
./build.sh linux/arm/7   # 選んだものだけ
```

`build.sh` が作るもの:

| 対象 | ファイル |
| --- | --- |
| macOS (Apple シリコン、Intel) | `pix68kcli-darwin-arm64`、`pix68kcli-darwin-amd64` |
| Linux (arm64、Raspberry Pi OS 32 ビット、x86-64) | `pix68kcli-linux-arm64`、`pix68kcli-linux-armv7`、`pix68kcli-linux-amd64` |
| Windows (x86-64、arm64) | `pix68kcli-windows-amd64.exe`、`pix68kcli-windows-arm64.exe` |

版は `PIX68KCLI_VERSION` (なければ `git describe`) を埋め込む。`pix68kcli version` で出る。

公開リポジトリの Releases には、この 7 つを付けている (タグを push したときに `.github/workflows/release.yml` が GitHub のホストの runner でビルドする。版はタグの名前)。
macOS と Linux (arm64、x86-64) では、Homebrew で入れられる (Releases のバイナリを使う。quarantine と実行の許可の手当ては要らない)。

```sh
brew install j3tm0t0/tap/pix68kcli
```

Releases から直接落とした場合は、実行の許可が付いていないので、macOS と Linux では `chmod +x pix68kcli-*` をしてから使う。macOS では、ブラウザなどで落としてファイルに quarantine の属性 (`com.apple.quarantine`) が付いた場合、署名していないので開けない。そのときは `xattr -d com.apple.quarantine pix68kcli-*` で外す (curl で落としたものには付かない)。

macOS は、Apple 以外のプログラムが LAN の機械につなぐのを「ローカルネットワーク」の許可で制限する (Homebrew で入れても同じ)。許可がないと、curl では届くのに
`no route to host` で失敗する (Homebrew の python も同じだった。`brew install j3tm0t0/tap/pix68kcli` で入れた v0.2.0 でも、許可のないアプリから動かすと、
curl は 200 を返す同じ宛先に `pix68kcli status` が `connect: no route to host` になった)。システム設定 > プライバシーとセキュリティ > ローカルネットワーク で、
pix68kcli を動かすターミナルのアプリを許可する。

## 宛先と token

設定ファイル `~/.config/pix68kcli/config.json` (Windows は `%AppData%\pix68kcli\config.json`。`$XDG_CONFIG_HOME` があればその下、`--config` か `PIX68KCLI_CONFIG` で別の場所) に、宛先へ名前を付けて書く。

```json
{
  "default": "pi3",
  "hosts": {
    "pi3": {"host": "192.168.1.x", "token_file": "~/.config/pix68kcli/pi3.token"},
    "pi4": {"host": "192.168.1.y", "token_file": "~/.config/pix68kcli/pi4.token"},
    "pi5": {"host": "x68000",      "token_file": "~/.config/pix68kcli/pi5.token"}
  }
}
```

- `host` はアドレスか `net_name=` の名前。口は `Host:` ヘッダがアドレスか `net_name=` の名前でないと断る (403) ので、ルータが付けるドメイン付きの名前 (`pix68k.lan` など) は使えない
- token は `net_token=` に書いた文字列。設定ファイルには値を書かず、`chmod 600` したファイルに 1 行で書いて `token_file` で指す (`token` に値を直接書くこともできるが勧めない)
- 知らない項目 (`host` を打ち間違えた `hots` など) があれば誤りにする

宛先と token の決まり方:

| | 優先する順 |
| --- | --- |
| 宛先 | `--host` > `PIX68K_HOST` > 設定ファイルの `default`。値が設定ファイルの `hosts` に書いた名前なら、その項目の `host` と token を使う。アドレスを書いても、同じ `host` の項目があれば、その token を使う |
| token | `--token-file` > `PIX68K_TOKEN` > `PIX68K_TOKEN_FILE` > 設定ファイルの `token_file` > `token` |

token は画面にもログにも出さない (`-v` も、送った要求の行に token の出どころだけを書く)。`pix68kcli config` で、決まった宛先と token の出どころ (値は出さない) を確かめられる。

プロキシは使わず (`HTTP_PROXY` を読まない)、口がリダイレクト (3xx) を返してもついて行かない。token を LAN の外に出さないため。

## 命令

```sh
pix68kcli status                        # つながるか、口の一覧、token が通るか、温度とクロック、Pi の型、速度
pix68kcli --host pi5 status             # 宛先を選ぶ
pix68kcli log                           # ログ (最新の 64KB)
pix68kcli log -n 20                     # 最後の 20 行
pix68kcli log -f                        # 追いかける (1 秒ごとに読む。再起動の間は待ち続ける)
pix68kcli screen                        # 画面を screen.png に保存
pix68kcli screen -o shot.ppm            # PPM のまま (.ppm)。- なら PNG を標準出力へ
pix68kcli thermal                       # /thermal をそのまま
pix68kcli reboot                        # ディスクと SRAM を書いてから再起動
pix68kcli reboot --profile gr2          # /pix68k/pix68k-gr2.cfg で再起動 (#231 のカーネルが要る)
pix68kcli reboot --profile=             # 組み合わせなし (pix68k.cfg だけ) で再起動
pix68kcli profiles                      # 起動の組み合わせ (/pix68k/pix68k-*.cfg) の名前の一覧
pix68kcli type 'dir\n'                  # キーを打つ。\n、\t、\b、\e は口が読むので、そのまま送る
pix68kcli type - < keys.txt             # 標準入力から
pix68kcli key down --mod 09 4c          # Ctrl + GUI + Delete (X68000 のリセット) を押したままにする
pix68kcli key up                        # すべて離す (key up 4c なら 1 つ)
pix68kcli message --sec 30 '案1\nいまの版'   # 画面の左上に 30 秒出す (既定 10 秒、0 なら消すまで)
pix68kcli message --clear               # 消す
pix68kcli files ls /pix68k              # SD カードの一覧
pix68kcli files get /pix68k/pix68k.cfg  # 読む (手元に pix68k.cfg を作る。2 つ目の引数で名前、- なら標準出力)
pix68kcli files put kernel8.img /       # 書く (/ で終われば手元の名前を付ける)
pix68kcli files put - /pix68k/pix68k.cfg < pix68k.cfg
pix68kcli files rm /pix68k/old.cfg      # 消す
pix68kcli api                           # 口の使い方の一覧 (GET /) をそのまま
```

- `status` の Pi の型は、起動したときのログの行 (`pix68k bare metal on ...`) から取る。X68000 を動かして 10 分ほどでログが 64KB を超え、その行が消えると「分からない」と出す
- 命令ごとのオプション (`--sec`、`--profile`、`-o` など) は、引数より前に書く
- `reboot --profile` は、先に `GET /` の一覧に `POST /reboot?profile=` の行があるか確かめ、なければ送らずに終了コード 7 で終える。
  #231 より前のカーネルは `profile=` を読まずに、そのまま `pix68k.cfg` で再起動してしまうため
- `files put` は `Content-Length` と `Expect: 100-continue` を付けて送る (口は chunked を断る。token や使っているイメージを確かめて
  断るときは、本体を送る前に 403 や 409 を受け取る)。大きなファイルでも時間切れにしない。
  `files get` は一時ファイルに書いてから名前を変えるので、途中で切れても (Ctrl+C や SIGTERM でも) 手元の元のファイルは残る。
  上書きするときは元のファイルの権限のまま、新しく作るときは umask に従う
- `files ls` にファイルを、`files get` にディレクトリを渡すと誤りにする (口が返す `Content-Type` で見分ける)
- `type` と `message` に `-` で始まる文字列を渡すときは、`--` の後ろに書く (`pix68kcli type -- '-l\n'`)
- `status` は、token が断られたら終了コード 4 で終える。口に `net_token=` がなく token なしでも読めるときは、「確かめていない」と出す
- `log -f` は `/log` を読むたびに、前に読んだ本文の終わりを探して、増えた分だけを出す。見つからなければ (再起動した、または 1 回の間に 64KB より多く増えた)、
    標準エラーに一言出してから、読んだ本文をすべて出す。読む間隔は `--interval 秒` (既定 1 秒)。口はログに要求ごとの 1 行 (`httpd: ... GET /log: 200`) を残すので、
  追いかけている間はその行を出さない (ほかの機械から読んだ行も出さない)。つながらないときと 503 のときは待ち続け (再起動の途中で始めてもよい)、
  403 や 404 のときは終える
- `type` は、口がキーを打ち終えてから返事をする (1 字に 80ms)。4096 バイトまで、`message` は 1024 バイトまで (超えれば送らない)

## 終了コードと誤りの文

| 終了コード | 意味 |
| --- | --- |
| 0 | 成功 |
| 1 | そのほかの失敗 (口の 400 や 500、手元のファイルの読み書き) |
| 2 | 使い方の誤り、設定ファイルの誤り、宛先がない |
| 3 | つながらない、時間切れ (つなぐのは 5 秒、ふつうの要求は 30 秒で切る) |
| 4 | 断られた (403。token が違う、`net_allow=`、`Host:`、書き込みを許していない) |
| 5 | ない (404。`files` のファイル、`reboot --profile` の組み合わせ) |
| 6 | 使用中か、動いていない (409 は X68000 が使っているディスクのイメージ、503 は設定メニューを開いている、X68000 が動いていない、など) |
| 7 | 動いているカーネルにその口がない (`thermal`、`screen`、`type`、`key`、`message` の 404 と、`reboot --profile` で一覧に `/reboot?profile=` がないとき) |

誤りは標準エラーに 1 行で出す。口が返した文 (英語) をそのまま付ける。

```
pix68kcli: 192.168.1.x: GET /files/: 403 bad token (X-Pix68k-Token, net_token=)
pix68kcli: 192.168.1.x: GET /files/nonexist: 404 no such file
pix68kcli: 192.168.1.x: GET /: つながらない: 時間切れ (dial tcp 192.168.1.x:80: i/o timeout)
pix68kcli: 192.168.1.x: GET /: つながらない: dial tcp 192.168.1.x:80: connect: connection refused
```

## 試験

```sh
cd tools/pix68kcli
go vet ./... && go test ./...
```

`httptest` で口の偽物を立て、各命令が送る要求 (メソッド、パスとクエリ、`X-Pix68k-Token`、本文、`Content-Length`) と、終了コード、誤りの文に token が出ないことを確かめる。CI (`.github/workflows/ci.yml` の `pix68kcli`) でも同じ試験とクロスビルドを行い、できたものを artifact に上げる。
