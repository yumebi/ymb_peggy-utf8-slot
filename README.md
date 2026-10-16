# peggy-utf8-slot

**Peggy Pro 4.63**（Anchor Systems製のテキストエディタ、2009年版）を、UTF-8 で使えるようにする非公式パッチ。

Peggy は内部が CP932(Shift_JIS)固定のため、CP932 に無い文字(絵文字、𠮷、中国語、ハングル、`〜` U+301C など)は、
読み込み時に `?` になり、保存で失われる。このパッチは、Peggy の速度・操作感・桁計算をそのままに、これらを扱えるようにする。

> English: an unofficial add-on that lets Peggy Pro 4.63 (a CP932-only Japanese text editor) read, display,
> type and save arbitrary Unicode (emoji, CJK, Hangul...) as UTF-8, by hooking the ANSI APIs of the unmodified
> binary through an injected DLL. No Peggy binaries are included; you patch your own licensed copy.

## 法的な注意
- 本リポジトリは **Peggy の実行ファイルを含まない**。Peggy は Anchor Systems(現在は事業終了)の著作物で、
  ビルドには **ご自身が正規に入手した Peggy Pro 4.63 の `peggypro.exe`** が必要。
- ビルド成果物(パッチ済み `peggypro.exe`、インストーラー)は、著作物の改変物にあたるため**再配布しないこと**。
- 試用期限などのライセンス確認処理には**一切手を加えていない**。

## 何ができるか
| | |
|---|---|
| 読み込み・保存 | UTF-8(BOM 有無)。BMP 外の文字(4 バイト UTF-8)も往復で完全一致 |
| 表示 | CP932 外の文字を 2 セル幅の枠に描画。絵文字は Segoe UI Emoji(白黒、GDI の制約) |
| 入力 | IME の変換結果、絵文字パネル等の Unicode 入力、Unicode クリップボードの貼り付け・コピー。検索窓などの標準 Edit コントロールへの貼り付けも、文書内と同じバイト列で入る(`〜` U+301C が `～` U+FF5E に化けず検索にヒットする) |
| ファイル名 | CP932 外の文字を含むファイル名で、開く・保存・「開く」「保存」ダイアログ・最近使ったファイルの履歴・ドラッグ&ドロップ・コマンドライン・タイトル/タブ表示 |
| 同一視検索 | 「文字列の検索」で「正規表現」をオンにすると、`〜`(U+301C)と`～`(U+FF5E)など、CP932 と Unicode で食い違う見た目が同じ文字(`〜～` `‖∥` `−－` `¢￠` `£￡` `¬￢` `—―` `¦￤`)を、パターンに `[〜～]` の形で自動展開して両方にヒットさせる(正規表現オフでは働かない)。ツールバーの検索窓も、検索ダイアログの「正規表現」の設定に従って同様に展開する |
| 表示 | ウィンドウタイトルは `Peggy Pro (UTF-8 Slot) - [...]`、バージョン情報は `Peggy Pro Version 4.63 (...) + UTF-8 Slot 1.3`。パッチ版だと分かる |
| 保存データの継続性 | スロット割り当ては `%APPDATA%\PeggyUtf8Slot\slots.dat` に保存され、次の起動でも同じ文字が同じスロットになる。Peggy が ANSI で書き出す検索履歴・プロジェクトファイルなどが、起動をまたいでも同じ文字として読める。表が満杯になったら、そのセッションで使っていないスロットを再利用する |
| 二重起動の転送 | 起動中の Peggy へ後から渡したファイル(`peggypro.exe ファイル名`)でも、CP932 外の文字を含む名前が正しく開く(スロット番号が起動ごとに違う問題を、本来の UTF-16 を添えて解決) |
| ファイルから検索 | 同梱の `pgrep.exe`(検索の実体)にも同じ DLL を読み込ませる。CP932 外の文字を含む UTF-8 / UTF-16 のファイルの本文(4 バイトの絵文字も)と、そうした文字を含むファイル名が、検索・結果表示とも正しく扱える。スロットは `slots.dat` 経由で Peggy と共有し、`pgrep.exe` が割り当てた分は結果を読む前に Peggy が取り込む |
| 互換 | 内部バッファは CP932 のまま。桁計算・速度・操作感は変わらない |

## 仕組み(スロット方式)
CP932 に無い文字を、CP932 の未定義 2 バイト領域(約 3556 個の「スロット」)へ動的に割り当てる。描画・保存時に本来の文字へ戻す。
スロットはキー入力の検証を通る**ユーザー定義領域(F0xx〜F9xx、約 1880 個)を優先**して割り当てる。

| ファイル | 役割 |
|---|---|
| `src/slot.c` | スロット表(割り当ての保存を含む)、UTF-16 ⇄ CP932 変換(`MultiByteToWideChar` / `WideCharToMultiByte` の置換) |
| `src/hook.c` | `DllMain`。exe の IAT を差し替える。スロット文字の描画、クリップボード整合 |
| `src/io.c` | 4 バイト UTF-8 の読み書き修復(読み込み時に CESU-8 化した一時ファイル、保存後に 4 バイトへ復元) |
| `src/fs.c` | Unicode ファイル名。スロット文字を含むパスだけ W 版 API (`CreateFileW`・`CreateProcessW` など) に切り替え、列挙結果・コマンドラインはスロット化して返す |
| `src/dlg.c` | Unicode コントロール(「文字列の検索」ダイアログなど)の ANSI での読み書き(`GetWindowTextA` / `SendMessageA` を W + スロット変換にする。ANSI の ComboBox は内側の Unicode の Edit を直接読む)。ファイルダイアログ(`GetOpenFileNameA` を W 版へ変換、`CDM_*` メッセージも変換)とレジストリ(REG_SZ を UTF-16 で保存)。MFC42 が遅延読み込みする API は `GetProcAddress` の横取りで差し替える |
| `src/pgrep.c` | `pgrep.exe` 側の組み込み(`fopen` の差し替え)と、Peggy 側でパイプを読む前のスロット取り込み |
| `src/ime.c` | IME 確定文字列(`WM_IME_COMPOSITION`)と Unicode パケット(`VK_PACKET`)の直接入力 |
| `tools/patch_import.py` | exe に `utf8slot.dll!DllInit` のインポートを追加(セクション追加なし)。`peggypro.exe` と `pgrep.exe` の両方に使う |
| `installer/peggy_utf8.iss` | Inno Setup 6 用インストーラー(元 exe のバックアップと、アンインストール時の復元つき) |
| `analysis/` | 解析に使った Ghidra スクリプト(結果の逆アセンブル出力は含めない) |

解析で分かった要点(4.63 / 2009-12-24 版):
- ロード関数 `0x4BF530`、UTF-8 リーダー(vtable `0x5AA7F4`)、UTF-8 ライタ `0x4CA030`
- 描画は関数ポインタ `[0x5F8560]`(既定 `TabbedTextOutA`)に集約
- Peggy は同梱の古い `Msvcrt.dll` を使うので、DLL から CRT の `FILE*` 関数を呼ぶと壊れる(Win32 API のみ使用)
- キー入力は CP932 の未定義セルを `・` に置換する(→ スロットにユーザー定義領域を優先使用)
- メッセージは ANSI 版 API で取得されるため、`VK_PACKET` の文字は低レベルキーボードフックでしか取れない

## ビルド
必要なもの: i686 ターゲットの mingw clang(例: llvm-mingw)、Python 3 + `pefile`、(任意)Inno Setup 6。

```bash
pip install pefile
./build.sh "/c/Program Files (x86)/Anchor/Peggy/peggypro.exe.bak"   # 無改造の 4.63 exe を指定(同じ場所の無改造の pgrep.exe も自動で使う。別の場所なら第 2 引数)
# 任意: インストーラー  ->  dist/PeggyUtf8Setup.exe
ISCC.exe installer/peggy_utf8.iss
```

`dist/peggypro.exe`・`dist/pgrep.exe`・`dist/utf8slot.dll` を Peggy のインストール先へ置く(元の exe は事前にバックアップすること)。

## 既知の制限
- 1 セッションで扱える CP932 外の異なる文字は約 3556 種(うち、キー・IME で直接入力できるのは約 1880 種)。過去のセッションの割り当ては、満杯のときだけ再利用される
- 永続化を始める前に Peggy が書いた検索履歴などは、割り当てが違うので、初回だけ別の文字に見えることがある。`slots.dat` を消すと、割り当てを最初からやり直す。無効化は環境変数 `SLOT_PERSIST=0`
- ファイル名: エクスプローラーの DDE 経由(`Peggy.Project` の `ddeexec`)で渡す経路は、Explorer が ANSI に落とすので文字が失われる(未対応)。外部コマンド(`CreateProcessA`)はコマンドライン・作業フォルダーのスロット文字を Unicode にして起動するが、起動された側が ANSI で出力する結果(grep の出力にあるファイル名など)は CP932 外の文字が `?` になる。プロジェクトファイルなど、Peggy が独自形式で ANSI のパスを書き出すものは、セッションをまたぐと文字が変わる可能性がある
- 検索ダイアログの入力欄は Unicode で表示される。ツールバーの検索窓は、作成時に Unicode 版で作り直しているので、CP932 外の文字も(絵文字はカラーで)表示される。それ以外の ANSI の入力欄に入った CP932 外の文字は、Windows の描画なので空白や四角で表示される(検索自体は動く)
- スロット文字は常に 2 セル幅。絵文字は白黒(カラー化には Direct2D が必要)
- 4.63(2009-12-24)専用。他のバージョンはアドレスが異なるため動作しない
- ファイル読み込み関数の位置は固定値(`src/io.c` の `LOAD_RVA_*`)

## デバッグ用の環境変数
| 変数 | 内容 |
|---|---|
| `SLOT_MASK` | 有効にするフックのビットマスク(切り分け用) |
| `SLOT_DEBUG` | ログの出力先ファイル |
| `SLOT_SELFTEST` | プロセス内の自己検証(起動 14 秒後)。`1`: IME 確定 / `ime`: IME をオン / `save`: `x` を入力して保存 / `open`: 「開く」ダイアログで `SLOT_SELFTEST_FILE` を開く / `drop`: `SLOT_SELFTEST_FILE` のドロップ / `proc`: `SLOT_SELFTEST_FILE` のパスを `CreateProcessA` 経由のコマンドラインに入れて起動 |
| `SLOT_FIND_REGEX` / `SLOT_FIND_MENU` / `SLOT_TB_TEXT` | 検索ダイアログの自己検証(`SLOT_SELFTEST=find`)用。`1`/`0` で「正規表現」をオン/オフ(`?` は報告のみ)。メニュー番号(既定 2、文書を開いた状態は 3)。`SLOT_TB_TEXT` を指定するとツールバーの検索窓で検索する(`@tilde` は `b` + `～`) |
| `SLOT_PERSIST` / `SLOT_PERSIST_FILE` | `0` でスロット割り当ての保存を無効化 / 保存ファイルの場所を変更(テスト用) |
| `SLOT_SELFTEST_FILE` | 上記テストで使うファイルのパス(Unicode 可) |
| `SLOT_SELFTEST_SAVE` | `SLOT_SELFTEST=1` の後に保存も行う |

## ライセンス
MIT License(`LICENSE` 参照)。**このリポジトリのソースコードにだけ適用される。** Peggy 本体(Anchor Systems の著作物)には適用されず、本リポジトリには含まれない。
