# peggy-utf8-slot

**Peggy Pro 4.63**(Anchor Systems 製のテキストエディタ、2009 年版)を、UTF-8 で使えるようにする非公式パッチ。

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
| 入力 | IME の変換結果、絵文字パネル等の Unicode 入力、Unicode クリップボードの貼り付け・コピー |
| ファイル名 | CP932 外の文字を含むファイル名で、開く・保存・「開く」「保存」ダイアログ・最近使ったファイルの履歴・ドラッグ&ドロップ・コマンドライン・タイトル/タブ表示 |
| 互換 | 内部バッファは CP932 のまま。桁計算・速度・操作感は変わらない |

## 仕組み(スロット方式)
CP932 に無い文字を、CP932 の未定義 2 バイト領域(約 3556 個の「スロット」)へ動的に割り当てる。描画・保存時に本来の文字へ戻す。
スロットはキー入力の検証を通る**ユーザー定義領域(F0xx〜F9xx、約 1880 個)を優先**して割り当てる。

| ファイル | 役割 |
|---|---|
| `src/slot.c` | スロット表、UTF-16 ⇄ CP932 変換(`MultiByteToWideChar` / `WideCharToMultiByte` の置換) |
| `src/hook.c` | `DllMain`。exe の IAT を差し替える。スロット文字の描画、クリップボード整合 |
| `src/io.c` | 4 バイト UTF-8 の読み書き修復(読み込み時に CESU-8 化した一時ファイル、保存後に 4 バイトへ復元) |
| `src/fs.c` | Unicode ファイル名。スロット文字を含むパスだけ W 版 API (`CreateFileW` など) に切り替え、列挙結果・コマンドラインはスロット化して返す |
| `src/dlg.c` | ファイルダイアログ(`GetOpenFileNameA` を W 版へ変換、`CDM_*` メッセージも変換)とレジストリ(REG_SZ を UTF-16 で保存)。MFC42 が遅延読み込みする API は `GetProcAddress` の横取りで差し替える |
| `src/ime.c` | IME 確定文字列(`WM_IME_COMPOSITION`)と Unicode パケット(`VK_PACKET`)の直接入力 |
| `tools/patch_import.py` | exe に `utf8slot.dll!DllInit` のインポートを追加(セクション追加なし) |
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
./build.sh "/c/Program Files (x86)/Anchor/Peggy/peggypro.exe.bak"   # 無改造の 4.63 exe を指定
# 任意: インストーラー  ->  dist/PeggyUtf8Setup.exe
ISCC.exe installer/peggy_utf8.iss
```

`dist/peggypro.exe` と `dist/utf8slot.dll` を Peggy のインストール先へ置く(元の exe は事前にバックアップすること)。

## 既知の制限
- 直接入力できる CP932 外の異なる文字は、1 セッションで約 1880 種まで(超過分は貼り付け・読み込みなら約 3556 種まで)
- ファイル名: エクスプローラーから起動中の Peggy へ渡す経路(二重起動の転送)と、外部コマンドの起動(`CreateProcessA`)は未確認・未対応。プロジェクトファイルなど、Peggy が独自形式で ANSI のパスを書き出すものは、セッションをまたぐと文字が変わる可能性がある
- スロット文字は常に 2 セル幅。絵文字は白黒(カラー化には Direct2D が必要)
- 4.63(2009-12-24)専用。他のバージョンはアドレスが異なるため動作しない
- ファイル読み込み関数の位置は固定値(`src/io.c` の `LOAD_RVA_*`)

## デバッグ用の環境変数
| 変数 | 内容 |
|---|---|
| `SLOT_MASK` | 有効にするフックのビットマスク(切り分け用) |
| `SLOT_DEBUG` | ログの出力先ファイル |
| `SLOT_SELFTEST` | プロセス内の自己検証(起動 14 秒後)。`1`: IME 確定 / `ime`: IME をオン / `save`: `x` を入力して保存 / `open`: 「開く」ダイアログで `SLOT_SELFTEST_FILE` を開く / `drop`: `SLOT_SELFTEST_FILE` のドロップ |
| `SLOT_SELFTEST_FILE` | 上記テストで使うファイルのパス(Unicode 可) |
| `SLOT_SELFTEST_SAVE` | `SLOT_SELFTEST=1` の後に保存も行う |

## ライセンス
MIT License(`LICENSE` 参照)。**このリポジトリのソースコードにだけ適用される。** Peggy 本体(Anchor Systems の著作物)には適用されず、本リポジトリには含まれない。
