<!-- Copyright 2026, OpenAI. SPDX-License-Identifier: BSL-1.0 -->
# Linux DRM/KMS ネイティブ出力（実験的）

`XRT_HAVE_DRM_KMS` は Linux、libdrm、Vulkan が利用可能な場合の条件付きオプションです。
既存の arpara Rust protocol/HID/IMU ドライバは変更しません。
`drm_kms` factory は明示指定専用で、自動選択しません。
VkDisplayKHR / VkSurfaceKHR / VkSwapchainKHR、X11、Wayland を使わず、peek window も生成しません。

Monado の DRM ターゲットは DMA-BUF 直接 scanout を使用します。CPU readback は独立プローブの旧診断オプションだけに残り、サービスは自動フォールバックしません。

```
Monado renderer -> exportable BGRA8 LINEAR VkImage (GENERAL)
 -> render_complete binary semaphore を消費
 -> GPU queue から FOREIGN queue へ ownership release + GPU fence wait
 -> DMA-BUF FD -> KMS GEM import -> XRGB8888 FB
 -> atomic commit -> page-flip 完了 -> 前の scanout image を再利用
```

描画画像 2 枚をそのまま KMS に登録し、staging buffer、CPU pixel mapping、全画面コピーを省きます。
Monado の通常の layer 合成/歪み描画は GPU 上で行います。GPU ドライバ内部の動作までゼロコピーと断定するものではありません。
LINEAR の single-plane modifier と実 renderer usage（compute では STORAGE）の exportability を照会し、
専用 Vulkan memory を割り当てます。offset/pitch は Vulkan の subresource layout から取得します。
KMS は明示 LINEAR modifier で AddFB2 を試し、EINVAL/EOPNOTSUPP の場合のみ、同じ LINEAR allocation の implicit AddFB2 を試します。
どちらも **実際の DMA-BUF framebuffer に対する atomic TEST_ONLY** を通過することが必要です。
事前の KMS 準備で使用する dumb buffer は診断/plane 選択用であり、サービスのフレーム転送には使用しません。

必要な device 拡張は `VK_KHR_external_memory_fd`、`VK_EXT_external_memory_dma_buf`、
`VK_EXT_image_drm_format_modifier`、`VK_EXT_queue_family_foreign` です。
FOREIGN は Vulkan GPU と異なる表示デバイス/ドライバに ownership を渡すために使います。
[仕様](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_queue_family_foreign.html)に従い、
対応がない場合に別の queue-family 定数で代用しません。

GPU 完了を CPU fence wait で確認してから KMS に渡し、page-flip 完了後だけ再利用します。
待機は約 3 秒を上限とし、失敗後は再利用を禁止します。queue は mutex で外部同期します。
まずコピー除去の実証を優先した同期方式であり、非同期 pipeline、KMS IN_FENCE_FD/OUT_FENCE_PTR、
精密な vblank pacing は後続課題です。60/90 Hz の安定駆動はまだ保証しません。
120 フレームごとに release/GPU 待ちと KMS flip 待ちの平均 CPU 経過時間をログに出します。
これにはアプリ側の描画や acquire 待ちを含まず、全体 FPS / motion-to-photon latency ではありません。

## Orange Pi 5 でのビルド

リポジトリの submodule は従来通り Rust protocol 実装を参照します。

```bash
git submodule update --init --recursive
# Debian/Armbian: 実行用 libdrm2 だけでは開発ファイルが不足します。
sudo apt install libdrm-dev libvulkan-dev
# 従来の Monado/Rust 依存も必要。
cmake -S . -B build -DXRT_HAVE_DRM_KMS=ON
cmake --build build --parallel 2
./build/src/xrt/targets/cli/monado-cli test
```

デスクトップ用バックエンドは既存構成でコンパイルされていても、DRM 選択時の表示経路には入りません。
完全に headless なビルドでは従来の XCB/Xlib/Wayland/peek オプションも無効化できます。

## Phase A: 列挙と atomic KMS テスト

まず読み取りと TEST_ONLY を実行します。表示操作を実行する前に既存画面の利用者と競合しないことを確認してください。

```bash
./build/src/xrt/targets/kms_probe/monado-kms-probe --list
./build/src/xrt/targets/kms_probe/monado-kms-probe --vulkan-list
./build/src/xrt/targets/kms_probe/monado-kms-probe \
  --device /dev/dri/card0 --connector DP-1 --mode 1920x1080@60.000 --test-only
```

card0/DP-1 は提示された実機ログの例です。カード、connector、モードは必ず `--list` の結果に合わせます。
モード名は `幅x高さ@Hz`（refresh は小数点以下 3 桁）の完全一致です。
arpara の適正表示モードをこの実装では確定しません。最初は列挙された現行モードで試してください。
引数なし / `--list` では列挙だけを行い、`--test-only` では実 modeset を行いません。

TEST_ONLY に成功してから、**一時的に実出力を変更する**テストを実行できます。
5 秒後または SIGINT/SIGTERM に元の connector/CRTC/plane atomic properties の復元を試みます。

```bash
./build/src/xrt/targets/kms_probe/monado-kms-probe \
  --device /dev/dri/card0 --connector DP-1 --mode 1920x1080@60.000 --show --seconds 5
```

TEST_ONLY は XRGB8888 linear dumb buffer を検査します。
primary plane が AFBC 専用なら、未使用の overlay plane も検査します。
`--plane ID` で候補を限定できます。共有/ミラー CRTC は変更を拒否します。
選択した connector/CRTC/plane、pitch、mode、format/modifier はログに表示します。
既存 framebuffer を所有する別クライアントを並行実行しないでください。

DRM master を取得できない場合、errno と理由を出して失敗します。
SSH セッションの tty だけではアクティブな seat の権限が付かない場合があります。
アクティブなローカル VT で既存 logind/seat ACL の範囲で試してください。
この実装は libseat/logind の device FD 取得・session pause/resume を実装していません。
権限設定の自動変更、恒久 root 運用、chmod 666 は行いません。

## Phase B: Vulkan 描画を KMS に渡す

`--vulkan-list` は GPU ごとの **device** 拡張を表示します。
DRM modifier と dma-buf 拡張がある GPU では、BGRA8 の modifier/plane count/format features、
COLOR_ATTACHMENT | TRANSFER_SRC の dma-buf image format query と import/export feature bits を照会します。
これは実際の export/import/scanout 成功を意味しません。
サービスの直接出力と新しい DMA-BUF プローブは LINEAR export/import を実装しています。
以下の旧 `--vulkan-show` は CPU コピー診断のままです。

```bash
./build/src/xrt/targets/kms_probe/monado-kms-probe \
  --device /dev/dri/card0 --connector DP-1 --mode 1920x1080@60.000 \
  --vulkan-show --gpu 0 --seconds 5
```

`--gpu` を省略した場合は ARM vendor の GPU を探します。CPU device は実出力テストで拒否します。
提示された GPU0 が Mali-G610/PanVK であることを出力で確認してください。
緑の Vulkan clear image を CPU readback して KMS に渡します。clear + readback の経過時間も出力します。
これは計測対象の解像度、GPU、ドライバに依存する単発値で、OpenXR の遅延を示しません。

DRM なしの Vulkan 検証（ここだけ CPU GPU を許可）:

```bash
./build/src/xrt/targets/kms_probe/monado-kms-probe --vulkan-readback-test --gpu 0
```

64x64 の全画素検査と、GENERAL layout の再コピー、binary semaphore の反復 signal/wait を行います。

## DMA-BUF の段階的な実機検証

サービスと他の KMS テストを終了してから、まず export と ownership を DRM なしで検証します。
`--dma-buf-check` は 64x64、STORAGE | TRANSFER_DST の画像 2 枚について 4 回の acquire/clear/release を行います。
同じ GPU index の device 拡張を確認し、非対応なら具体的な拡張名とともに失敗します。

```bash
./build/src/xrt/targets/kms_probe/monado-kms-probe --dma-buf-check --gpu 0
./build/src/xrt/targets/kms_probe/monado-kms-probe \
  --device /dev/dri/card0 --connector DP-1 --mode 3840x1920@71.998 \
  --dma-buf-test-only --gpu 0
```

TEST_ONLY が通ったら、実際に表示を変更する以下のテストで緑色の画像を確認します。
GPU が描画した共有画像を交互に scanout し、平均 FPS / GPU release / flip 待ち時間を出します。
これは単純な clear の試験で、hello_xr の FPS を示すものではありません。

```bash
./build/src/xrt/targets/kms_probe/monado-kms-probe \
  --device /dev/dri/card0 --connector DP-1 --mode 3840x1920@71.998 \
  --dma-buf-show --gpu 0 --seconds 30
```

通常終了 / SIGINT / SIGTERM では KMS 復元と FB/GEM 解放を行ってから Vulkan memory を解放します。
CPU GPU は物理表示テストで拒否します。SIGKILL/電源断や master 喪失時の復元は保証できません。

確認できたらサービスを従来の起動手順で実行します。追加の DMA-BUF 有効化変数は不要です。
90 Hz のモードは提示された EDID の `3840x1920@90.001`、60 Hz の例は `2560x1440@59.999` です。
3840x1920@60 は提示された EDID にありません。各モードは TEST_ONLY から確認してください。
画面比率/FOV/レンズ校正はこの DMA-BUF 変更では確定しません。

## Phase C: Monado renderer と接続

単体テストの表示を確認し、ユーティリティを終了してからサービスを起動します。
以下は現行の 1920x1080 モードを明示的に使用する診断例です。

```bash
XRT_COMPOSITOR_FORCE_DRM=1 \
XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 \
XRT_COMPOSITOR_DRM_MODE=1920x1080@60.000 \
XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
XRT_COMPOSITOR_LOG=debug \
./build/src/xrt/targets/service/monado-service
```

connector と mode は必須です。device を省略すると KMS card を探索します。
HMD render extent と物理モードの比率が異なると起動を拒否します。
`XRT_COMPOSITOR_DRM_ALLOW_STRETCH=1` は**明示的な診断用非等倍変形**を許可し、警告します。
例えば暫定 2560x1440 と 3840x1920 は比率が異なります。
この許可は正しい光学補正ではありません。
HMD driver の仮値を実仕様として書き換えることなく、compositor output extent を KMS mode に合わせます。

サービスが起動しても、OpenXR クライアントの描画を arpara の物理 DP 出力で確認するまでは表示成功ではありません。
通常終了では復元を試みます。hotplug/master loss の復元は失敗し得ます。SIGKILL、クラッシュ、
電源断に対する復元保証はありません。復元失敗時は明確なエラーを出します。
異常時の外部監視による復元、seat lifecycle、長時間安定性は実機で追加検証が必要です。

## 確認状態

ユーザーの Orange Pi 5 実機ログでは、従来の CPU コピー経路について以下を確認しています。

- rockchip card0 / DP-1 / connector 84 / CRTC 82 / plane 39。
- 3840x1920@71.998、XRGB8888 LINEAR、dumb pitch=15360 の TEST_ONLY 成功。
- 青/オレンジのチェック柄が物理 HMD に表示され、終了後の atomic state 復元に成功。
- Mali-G610/PanVK の clear + CPU readback、hello_xr の物理表示成功（フレームレートにばらつきあり）。
- BGRA8 LINEAR、COLOR_ATTACHMENT | TRANSFER_SRC の export/import query は成功。
  STORAGE usage と FOREIGN 対応、実 DMA-BUF export/scanout の確認を代替するものではありません。

今回の DMA-BUF 変更のローカル検証:

- 既存 x86_64 `build/` の全体ビルド成功。
- arpara/Rust/queue mutex 回帰、および DMA-BUF ownership / FD cleanup / timeout の GPU-free fault injection を検証。
- AArch64 GCC 14.3 で新しい DMA-BUF module と変更モジュールの object compile 成功。
  今回の DMA-BUF 経路を ARM ライブラリへ link して実行したとは報告しません。
- SwiftShader は DMA-BUF/modifier 拡張を持たず、明確な非対応エラーで停止。共有の成功とは扱いません。
- ローカルに `/dev/dri` と HMD がなく、**新 DMA-BUF 経路の TEST_ONLY、物理表示、PanVK/hello_xr 性能は未検証**。
  CLI の Simulated HMD は未接続時の既存挙動であり、arpara 回帰成功の証拠にはしません。

ログは `build/kms-validation/dma-*.log` に保存します（Git 管理対象外）。
残る課題は、実機での DMA-BUF 表示確認、LINEAR 非対応時の modifier 交渉拡張、非同期 fence/pacing、
画面比率と光学校正です。arpara Rust/HID/IMU コードは変更していません。

## hello_xr の更新 FPS とフレーム間隔の測定

サービスの `KMS scanout:` 行は、kernel page-flip event の timestamp と sequence に基づきます。
CPU の present 呼び出し回数ではなく、完了した framebuffer 更新の間隔 120 個で集計します。

```bash
cmake --build build --parallel 2
XRT_COMPOSITOR_FORCE_DRM=1 \
XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 \
XRT_COMPOSITOR_DRM_MODE=3840x1920@90.001 \
XRT_COMPOSITOR_DRM_ALLOW_STRETCH=1 \
XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
XRT_COMPOSITOR_LOG=info \
./build/src/xrt/targets/service/monado-service 2>&1 | tee /tmp/monado-drm-90hz.log
```

別のターミナルで従来通り hello_xr を起動します。同じ場面・画質・GPU・姿勢で、起動直後の約5秒を除き、
60秒程度のログを採取してください。hello_xr を終了しサービスを Ctrl-C で終了します。
72 Hz と比較するときはモードを `3840x1920@71.998`、保存先を別名にします。

ログの形式（値は例ではなく項目名の説明）:

- `mode`: 選択した KMS モードの refresh。アプリ FPS ではありません。
- `update_fps`: 120 / page-flip 間隔の合計秒数。コンポジタが実際に FB を更新した頻度です。
  同じアプリ画像の再合成も数えるため、アプリが生成した新規フレーム数とは異なります。
- `interval_ms[min/p50/p95/max]`: イベント間隔の最小値、中央値、nearest-rank の95パーセンタイル、最大値。
  各約120区間の集計です。ブロックの p95 を平均して全測定期間の p95 として扱わないでください。
- `vblanks_without_update`: 隣接イベントの sequence 差から計算した、FB 更新を行わなかった表示周期数。
  低いアプリ更新率、pacing、GPU 待ちなどを区別せず数えます。アプリの dropped frame 数ではありません。
- `multi_vblank_intervals`: 2表示周期以上をまたいだ更新間隔の数。
  sequence を取得できない場合 `vblanks_without_update=-1` になり、sequence に基づく値は評価できません。

90 Hz なら1周期は約11.111 ms、2周期は約22.222 msです。
更新 FPS が90に近く、p95も1周期に近く、更新なし周期が少ないかを確認します。
これは判定基準であり、今回の実機結果を予言するものではありません。
1秒超の pause または timestamp の後退は不連続と扱い、途中の集計窓を破棄します。
そのため長い停止を含む全体の平均 FPS は、このログから算出しません。

イベント間の時刻差だけを使い、CPU/Vulkan の時計との一致を仮定しません。
KMS timestamp は kernel が報告する flip 時刻で、HMD の実発光時刻、姿勢から表示までの遅延、
パネルの残像は測定しません。Mali-G610 の提示ログでは GPU timestamp が利用不可なので、
従来の `release/GPU wait` は CPU が計った queue 完了待ち時間として扱います。

計測コードの回帰テストには、90 Hz相当のイベント列、1周期の更新抜け、sequence の32bit wrap、
pause、時刻後退と sequence がない場合を含めています。
