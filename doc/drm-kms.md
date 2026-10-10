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
 -> GPU queue から FOREIGN queue へ ownership release + sync FD export
 -> DMA-BUF FD -> KMS GEM import -> XRGB8888 FB
 -> IN_FENCE_FD または DMA-BUF write fence -> atomic commit -> page-flip 完了 -> 前の scanout image を再利用
```

サービスは描画画像 3 枚をそのまま KMS に登録し、staging buffer、CPU pixel mapping、全画面コピーを省きます。
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

acquire ownership barrier は renderer と同じ queue に非同期 submit し、CPU fence wait を行いません。
acquire と release の command buffer は別です。release は renderer の binary semaphore を1回消費し、
FOREIGN ownership barrier の完了 fence を `VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT` で export します。
選択 plane の `IN_FENCE_FD` があれば、nonblocking atomic commit に sync FD を添付します。
初期の modetest ログではこの property を確認できなかったため、Rockchip では DMA-BUF reservation の
`DMA_BUF_IOCTL_IMPORT_SYNC_FILE`（WRITE）経路も実装しました。
Linux 6.18 の [atomic helper](https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/drm_atomic_helper.c) は
GEM plane の prepare_fb が未指定の場合に reservation fence を抽出します。
[Rockchip VOP2](https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/rockchip/rockchip_drm_vop2.c) はこの経路を使います。
他の KMS driver に同じ動作を仮定せず、暗黙同期経路は driver 名 `rockchip` に限定します。
初期化時には EXPORT_SYNC_FILE ioctl を照会し、フレームごとの IMPORT_SYNC_FILE が失敗した場合は
未同期の画面を commit せず停止します。ioctl 成功だけで実機 scanout 成功とは判定しません。

sync FD 経路には `VK_KHR_external_fence_fd` の有効化と SYNC_FD の exportability が必要です。
`XRT_COMPOSITOR_DRM_SYNC_FD` が未指定なら対応判定して選択し、未対応なら既存の
DMA-BUF＋CPU release fence 待ちを維持します。`1` は新経路を必須にし、未対応なら起動を失敗させます。
`0` は比較用に従来の待ちを明示選択します。いずれも CPU pixel copy は行いません。
[SYNC_FD export](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetFenceFdKHR.html) は fence payload を転送するため、
元の Vulkan fence で完了を判定し直しません。返却 FD が `-1` なら既に完了しており、待ちを追加しません。
FD は atomic ioctl / reservation import の後に success/failure とも close します。

KMS commit は1件だけ pending にし、scanout / queued / free の3状態を区別します。
export 済み画像は、その fenced commit の完了に加え、後続 page-flip で表示から外れたことを確認してから
command buffer/fence を再利用します。失敗時には再利用せず、終了処理で GPU idle を待ちます。
present 後の renderer の強制 acquire/wait は DRM で省き、次フレームと表示待ちを重ねます。
**renderer の共有 descriptor pool / mapped UBO / command buffer を保護する GPU fence 待ちは残ります。**
非同期 present ではその待ちを描画準備より前に置き、GPU 使用中の共有リソースを書き換えません。
GPU 待ちの全面除去・複数フレームの同時合成には、これらもフレームごとに分離する必要があります。
独立プローブの従来の present API は同期動作を維持します。
待機は約 3 秒を上限とし、失敗後は再利用を禁止します。queue は mutex で外部同期します。
予測表示時刻には直近の page-flip 位相を使います。CPU fence 待ち経路では wake→GPU-ready の
平滑化した実測予算（1 ms margin）、sync FD 経路では保守的に1表示周期を予算とします。
後者の submit/export 時間は GPU 完了時間ではないので、描画予算に代入しません。
GPU-bound の queue に fake pacer の固定3 ms予算による待機を加えず、wake は即時にし、FIFO backpressure で制御します。
DRM_CAP_TIMESTAMP_MONOTONIC がある場合は KMS event timestamp を使い、ない場合はイベント受信時の
CPU monotonic 時刻を使ってログに明示します。後者は通知遅延を含みます。
光学的な present-to-photon offset は未校正です。60/90 Hz の安定駆動はまだ保証しません。
120 フレームごとに release submit/export（従来経路では release/GPU wait）、前の flip drain/commit の
平均 CPU 経過時間と prediction budget、CPU release fence wait の回数をログに出します。後者の flip 時間は現在の commit の完了待ちではありません。
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
./build/src/xrt/targets/service/monado-service 2>&1 | tee -i /tmp/monado-drm-90hz.log
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

## 転送ログの比較と FIFO pipeline 変更

`/home/reina/ArparaOpiLog/monado-drm-72hz.log` と `monado-drm-90hz.log` 全体を解析しました。
各集計窓の区間数が120で等しいため、全体の更新率は窓の FPS の調和平均で求めています。
起動時の窓を含み、丸められたログ値からの概算です。窓のp95は平均していません。

| モード | 窓数 | 対象区間数 | 対象時間（概算） | 更新率 | GPU release/queue wait平均 | flip wait平均 |
|---|---:|---:|---:|---:|---:|---:|
| 71.998 Hz | 6 | 720 | 20.03秒 | 35.95 FPS | 10.76 ms | 3.40 ms |
| 90.001 Hz | 14 | 1680 | 48.59秒 | 34.58 FPS | 10.81 ms | 10.23 ms |

72 Hz の後半はほぼ2周期ごとの更新、90 Hz は主に2〜3周期ごとの更新でした。
転送ログの末尾には終了時の復元行がなく、今回のファイルだけでは終了時復元を確認できません。
ログ保存には `tee -i` を使用すると、Ctrl-C 後もサービス終了処理の出力を保存できます。

ソース調査では acquire の CPU fence wait、同期 page-flip 待ち、renderer の present 後の acquire、
fake pacer の固定3 ms予算を確認しました。今回の変更は上記の3画像 FIFO pipeline と即時 wake です。
画像の ownership release/acquire、render_complete binary semaphore の1回消費、KMS TEST_ONLY は維持します。
停止時には pending flip を drain してから復元と FB/GEM 解放を行います。
idle中に完了済みイベントを後から回収しても、通知を処理しただけで timeout と誤判定しないようにしています。

ローカルでは全体ビルド、arpara/Rust、DMA-BUF ownership/timeout、非同期 acquire の待機数と別 command buffer、
3画像の scanout/queued 保護、vblank位相と描画予算の予測テストが成功しています。
AArch64 は変更モジュールの object compile まで確認しました。
下記の追加ログで pipeline の実機更新率を確認しました。画面比率は今回変更していません。

比較時は同じ hello_xr 設定で、上記コマンドの mode をまず `3840x1920@71.998`、保存先を
`/tmp/monado-drm-pipeline-72hz.log` にして採取し、次に90 Hzで
`/tmp/monado-drm-pipeline-90hz.log` を採取してください。`KMS scanout` の update_fps/p95/更新なし周期で比較します。


## release fence の CPU 待ち改善と再測定

ユーザー提供 `ArparaOpiLog/1` のログ（`1c737dbf2`）は以下でした。
更新率は窓の FPS の調和平均、起動窓を含む丸め値からの概算です。

| モード | 対象区間 | 更新率 | 複数vblank間隔 | 更新なしvblank |
|---|---:|---:|---:|---:|
| 71.998 Hz | 1680 | 71.96 FPS | 1 / 1680 | 1 |
| 90.001 Hz | 3120 | 86.56 FPS | 124 / 3120 | 124 |

追加の実機ログ `ArparaOpiLog/2` で sync FD 経路の KMS 更新率を確認しました（下記）。
終了時復元と光学的な遅延・残像は、このログでは未確認です。
ローカルには DRM card / PanVK / HMD がないため、コンパイルと fault injection で検証しています。
`syncfd-build.log`、`syncfd-tests.log`、`syncfd-arm.log` は `build/kms-validation/` に保存します。
全体ビルドと CTest **32/32** が成功しました。テストには sync FD export の CPU 待ちゼロ、
表示退役前の再利用禁止、already-signaled FD=-1、export/atomic/property/import 失敗時の安全停止と
FD 解放、Rockchip write fence の登録を含みます。`monado-cli test` も終了コード0でしたが、
ローカルに HMD がないため実機 arpara の認識/IMU 回帰を確認したものではありません。
AArch64 は変更 C モジュールの object compile であり、Orange Pi 上の全体リンク/実行ではありません。

既存のテスト手順に `XRT_COMPOSITOR_DRM_SYNC_FD=1` を追加します。

```bash
cmake --build build --parallel 2
XRT_COMPOSITOR_FORCE_DRM=1 \
XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 \
XRT_COMPOSITOR_DRM_MODE=3840x1920@90.001 \
XRT_COMPOSITOR_DRM_ALLOW_STRETCH=1 \
XRT_COMPOSITOR_DRM_SYNC_FD=1 \
XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
XRT_COMPOSITOR_LOG=info \
./build/src/xrt/targets/service/monado-service \
  2>&1 | tee -i /tmp/monado-drm-syncfd-90hz.log
```

別ターミナルで従来の hello_xr を同じ設定で動かし、約60秒採取します。
`DRM synchronization: Vulkan sync FD -> ...; CPU release fence waits=0` と
120フレームごとの `release submit/export` を確認してください。これは CPU 側の submit/export 時間です。
`KMS scanout` の update_fps / p95 / vblanks_without_update で表示更新を比較します。
比較対象は `XRT_COMPOSITOR_DRM_SYNC_FD=0`、保存先 `/tmp/monado-drm-cpufence-90hz.log` とします。
72 Hz も同じ手順で mode を `3840x1920@71.998` に変えます。
最後は hello_xr とサービスを終了し、`Previous atomic KMS state restored` を確認してください。
新経路に失敗した場合はエラー行を含むログを保存し、`0` で既存経路へ戻せます。
画面比率・光学校正・IMU はこの変更の対象にしていません。


## sync FD 実機測定（ArparaOpiLog/2）

`608615e4c` のログ全体を集計しました。GPU は Mali-G610、KMS は Rockchip、
connector=84 / CRTC=82 / plane=39、3840×1920、XRGB8888 / LINEAR、pitch=15360、画像3枚です。
**選択 plane の IN_FENCE_FD は実機で supported と確認でき、実際にこの経路が選ばれています。**
DMA-BUF reservation による暗黙同期の代替経路は今回使われておらず、実機未検証です。
CPU pixel copy と CPU release fence wait はどちらも0と記録されています。

| モード | 集計窓 | 対象区間 | 対象時間（概算） | 更新率 | 複数vblank間隔 / 対象区間 | release submit/export 平均 |
|---|---:|---:|---:|---:|---:|---:|
| 71.998 Hz | 19 | 2280 | 31.68秒 | 71.97 FPS | 1 / 2280（0.044%） | 0.175 ms |
| 90.001 Hz | 22 | 2640 | 29.91秒 | 88.26 FPS | 52 / 2640（1.970%） | 0.162 ms |

区間数で重み付けした更新率を算出し、起動窓を含めています。記録された丸め値からの概算です。
72 Hz の更新抜け1回は最初の集計窓で、その後18窓では更新抜け0でした。
90 Hz は前回 `ArparaOpiLog/1` の86.56 FPS / 3.974%から、今回88.26 FPS / 1.970%です。
短時間・異なる採取期間の比較で、すべての負荷条件で同じ改善や90 FPS維持を保証しません。

各窓の中央値は72 Hzで13.889 ms、90 Hzで11.111 msでした。
窓ごとのp95範囲はそれぞれ13.891〜13.958 ms、11.116〜11.185 msです。
全測定期間のp95はこの集計済みログから再構成できません。
最大更新間隔は72 Hzで27.773 ms、90 Hzで22.289 msで、90 Hzには2周期の更新間隔が残ります。

従来の約10.65 / 10.85 msはCPUによるGPU完了待ち、新しい0.175 / 0.162 msは
release submission と sync FD export のCPU経過時間です。GPU描画時間の短縮とは解釈しません。
前の flip drain/commit は72 Hzで平均13.126 ms、90 Hzで5.444 msでした。
新経路ではGPU完了を待つ前にcommitできるため、この待機位置と重なり方が変わります。
数値が増えたことだけで性能低下と判断せず、KMSイベント間隔と更新抜けで評価します。

両ログに ERROR / timeout はありません。どちらにも `Previous atomic KMS state restored` と
`Server exiting` の行がないため、終了時復元の成否は確認できません。
次の採取では `tee -i` を使い、hello_xrを止めてからサービスをCtrl-Cで終了し、復元行を保存してください。
残る90 Hz更新抜けの切り分けには renderer の共有リソース保護によるfence待ち、CPU描画準備、
GPU合成とvblankの位相を別々に計測する必要があります。共有リソースを保護する待ちは現時点で維持しています。
解析結果は `build/kms-validation/syncfd-hardware-summary.json` に保存しています。


## 正常終了を含む追加確認（ArparaOpiLog/3）

90.001 Hz、`608615e4c` のログ全体を確認しました。
今回も Mali-G610 / Rockchip / IN_FENCE_FD が選択され、CPU release fence wait / pixel copy は0です。
終了部に `Previous atomic KMS state restored` と `Server exiting: '0'` があり、
**この実行のKMS復元とサービス正常終了をログで確認できました。** ERROR / timeout はありません。

| 対象 | 区間数 | 対象時間（概算） | 更新率 | 複数vblank間隔 | 更新なしvblank | 最大更新間隔 |
|---|---:|---:|---:|---:|---:|---:|
| 全19窓 | 2280 | 26.34秒 | 86.55 FPS | 41 | 91 | 577.718 ms |
| 最初の窓を除く18窓 | 2160 | 24.41秒 | 88.49 FPS | 37（1.713%） | 37 | 22.318 ms |

最初の窓に約578 msの長い更新間隔があり、全体の平均を下げています。
この間隔の原因はログだけでは特定できません。最初の窓を除く値を全体の値として扱わないでください。
同窓には複数vblank間隔4件・更新なし54周期があり、長い停止1回は多くの表示周期を含みます。
残り18窓は前回の約88.26 FPSに近い値ですが、2周期ごとの更新は残っています。
全窓のp95は11.113〜11.184 ms、中央値は11.111 msで、これらは各窓の統計値です。
release submit/export は全19窓の平均0.163 ms、前のflip drain/commitは5.300 msでした。
解析結果は `build/kms-validation/syncfd-hardware-run3.json` に保存しています。
このログ確認ではソース・表示経路を変更しておらず、ビルド/テストの再実行は行っていません。


## 今後改善の可能性（当面は現状維持）

2026-10-10 時点では、ユーザーが現在の動作を十分と判断したため、性能改修はここで一区切りとします。
この節は後続作業の候補であり、実装済み・効果確認済みという意味ではありません。

現状は Orange Pi 5 / Mali-G610 / PanVK → BGRA8 LINEAR DMA-BUF → Rockchip KMS → arpara の経路です。
3画像、IN_FENCE_FD、CPU pixel copy / release fence wait とも0、正常終了時のKMS復元を確認しています。
72 Hz は約71.97 FPS、90 Hz は約88.26〜88.49 FPS（後者の88.49は最初の集計窓を除く値）でした。
90 Hzの完全維持、光学的な遅延・残像、IMU追跡とレンズ校正を達成したとは扱いません。
再開時も既存の Rust protocol/HID/IMU、非WSI表示経路、成功済みの同期・再利用条件を保全します。

| 優先 | 候補 | 期待すること・最初の検証 |
|---|---|---|
| 1 | 残る待ち時間を分けて計測 | renderer の共有リソース保護のfence待ち、CPU描画準備、queue submit、KMS commit/eventを別々に記録する。90 Hzで2周期になるフレームと対応付け、最初の約578 msの長い間隔の原因も調べる。 |
| 2 | renderer リソースをフレームごとに分離 | descriptor pool、mapped UBO、command buffer、scratch画像、query poolなどの共有箇所と寿命を先に棚卸しする。GPU使用中のCPU書き換えを防ぎ、各フレームのfenceとscanout退役で管理できれば、次フレームの準備を重ねられる可能性がある。単純に現在のfence待ちを削除しない。 |
| 3 | vblank位相とpacingを改善 | 現在の即時wake・1周期の予測予算を基準に、実測したGPU完了/flip情報でdeadlineと描画開始を調整する。更新抜けと予測誤差を同時に評価し、FPSだけのために先行フレーム数を増やさない。光学的なpresent-to-photon offsetは別途校正が必要。 |
| 4 | modifier交渉・メモリ帯域の改善 | LINEAR以外のformat/modifierをPanVKとKMSで照会し、実renderer usageでのexportability、plane layout、import、TEST_ONLYを確認する。共通候補があれば帯域と合成時間を比較する。AFBC対応や効果は未確認で、stride/offsetの推測やBSPへの移行は行わない。 |
| 5 | 描画負荷と画面比率を整理 | 同じ物理モードを保ち、アプリの推奨描画解像度・合成負荷を段階的に比較してGPU側の余裕を測る。HMDの暫定2560×1440と出力3840×1920を仕様として同一視せず、左右のviewport・FOV・表示配置を調べてALLOW_STRETCH依存を解消する。画質低下を性能向上と混同しない。 |
| 6 | 長時間・異常系の検証 | 新規起動、クライアント停止/再接続、SIGINT、hotplug、master喪失、device lossとcommit失敗を個別に確認する。FD/GEM/fenceのリークと再利用を検査し、復元不能時は失敗理由を記録する。Rockchipのreservation同期代替経路はIN_FENCE_FD経路と別に実機検証する。 |

パネルの残像・モーションブラーや姿勢から表示までの遅延は、KMSの更新FPSだけでは判断できません。
必要になった段階で光学計測を検討し、アプリの新規フレーム率、コンポジタの再合成率、物理refreshを区別します。
正確なレンズ歪みを扱う場合は、配置を確認して `ArparaHIDProtocolReverseEnginiering/arpara-original.json`
などの資料を参照し、値の出典と対象個体・表示モードへの適合性を確認します。

再開時の比較条件は、同じhello_xr設定・GPU0・3840×1920、72/90 Hzを別々に測定することです。
各試行は60秒以上を目安に、起動を含む全体と開始5秒後の区間を分け、`tee -i`で正常終了まで保存します。
区間を厳密に切り出す必要がある場合はフレームごとの時刻記録を追加します。
現在の120区間ごとの集計ログから全期間のp95や任意の5秒境界を再構成しません。
比較対象は `XRT_COMPOSITOR_DRM_SYNC_FD=0/1`、指標はKMS update_fps、更新なしvblank、
更新間隔の分布、CPU待ち時間、必要に応じて光学的な遅延です。
バッファ数を増やす場合は、遅延増加とreprojection時の姿勢の鮮度も評価します。
ユーザー権限・seat・OS設定は無断で変更せず、各変更の効果を確認してから次の候補へ進みます。

このメモの追加ではソース変更・ビルド・テストの再実行は行っていません。

## Mesa 26対応と3840×1920出力geometry（2026-10-10）

Mesa更新後のユーザーログでは Mali-G610 MC4 / Vulkan 1.4.354 が選ばれ、
3画像のDMA-BUF importとTEST_ONLY、sync FD初期化までは成功しました。
その後 `comp_vulkan_formats_copy_to_info` が停止しており、OpenXR表示成功とは扱いません。

Monadoが検査するVulkan swapchain format候補は19種類ですが、従来の配列上限は16でした。
旧処理はassertionより先に配列へ書き込んでいました。上限を64に拡張し、
書き込み前に上限を確認するよう修正しました。将来候補が64を超えた場合は、
元の優先順で64種類まで公開し、省略数を警告します。実際の対応数は起動時に
`Vulkan swapchain formats: supported=N advertised=M capacity=64` と記録します。
新Mesa実機の対応数は従来ログからは確定できません（上限超過の17〜19種類が候補）。
19種類すべて対応するケースはGPU不要の回帰テストで確認します。

`xrt_compositor_info.formats`、target要求format配列、settings、OpenXRのfilter配列は
同じ定数を使用しています。IPC compositor_get_infoの返信は構造体のsizeofで送受信されるため、
構造体サイズの変更が両端に反映されます。IPC受信側とOpenXR列挙側にもcount検証を追加しました。
**サービスとMonado OpenXRランタイム／クライアントライブラリを同じ版で全体再ビルドしてください。**
古いサービスを終了し、更新したランタイムをhello_xrから使用します。
`IPC_IGNORE_VERSION=1` で旧版との不一致を回避しないでください。

arpara USB HIDドライバが報告する暫定SBS geometryを確認したうえで、DRM専用に
3840×1920の出力profileを設定します。左viewportは(0,0,1920,1920)、右は
(1920,0,1920,1920)です。最終合成画像とKMS framebufferは選択モードと同じ3840×1920で、
中間画像・コピーを追加しません。refreshは選択モードから取得し、72/90 Hzで同じgeometryです。
アプリ向け推奨画像サイズを決めるHMDの `views[].display` は変更しません。
非DRMバックエンドと他HMDは従来のviewport計算を使用します。

このprofileは、既存ドライバの2眼・等幅SBS・回転なしという配置を保つためのものです。
資料には各眼2544×2544という別の記述がありますが、3840×1920モードの実際の眼順序を
証明する情報は確認できませんでした。**物理左右配置、パネル本来の解像度、レンズ歪み、
FOV、IPDの校正が確定したという意味ではありません。** hello_xrで各眼の表示位置と
幾何学形状を実機確認してください。profileに一致しないgeometry／モードでは従来の
比率チェックを維持し、暗黙にストレッチしません。

再ビルドと実機テスト（通常ユーザー、既存のDRM master/seatアクセスを使用）:

```sh
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/src/xrt/targets/cli/monado-cli test

# ALLOW_STRETCHを外し、まず90 Hz。別試行では MODE を3840x1920@71.998に変更。
env -u XRT_COMPOSITOR_DRM_ALLOW_STRETCH \
  XRT_COMPOSITOR_DRM_SYNC_FD=1 \
  XRT_COMPOSITOR_FORCE_DRM=1 \
  XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
  XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 \
  XRT_COMPOSITOR_DRM_MODE=3840x1920@90.001 \
  XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
  XRT_COMPOSITOR_LOG=info \
  ./build/src/xrt/targets/service/monado-service \
  2>&1 | tee -i /tmp/monado-drm-mesa26-90hz.log
```

別端末からこれまでのhello_xrを起動し、60秒以上測定してください。
`DRM output geometry` と `DRM final render extent=3840x1920, framebuffer=3840x1920`
を確認します。DMA-BUF BGRA8/XRGB8888 LINEAR、3画像、IN_FENCE_FD、CPU pixel copies=0、
CPU release fence waits=0、終了時KMS復元も確認します。
今回ownership transfer、FIFO、fence、scanout退役、atomic復元処理は変更していません。
ローカル開発環境では実機表示、実機対応format数、72/90 Hzの更新率は未検証です。

今回のローカル検証結果:

- `cmake --build build --parallel 2`: 全体ビルド成功。変更したヘッダの依存先を含め再構築。
- `ctest --test-dir build --output-on-failure`: 34/34成功（2.79秒）。追加format/geometry、
  既存DMA-BUF、KMS fence、FIFO/timing、arpara fixedとRust protocolテストを含む。
- format全19種類のテスト: 9 assertions成功。GPUの列挙実測ではなく、全候補対応時の境界回帰確認。
- `monado-cli test`: 終了0。ただし開発機に実機HMDはなく、Simulated HMDの結果。
  arpara実機のHID/IMU成功の証拠にはしない。
- AArch64コンパイル: DRM/KMS、DMA-BUF、target、renderer、settings、compositor、
  Vulkan format処理、IPC client、OpenXR sessionを含むCオブジェクトのコンパイル成功。
  ARM向け全体リンク・実行は未検証。
- `git diff --check`: 成功。

ログは `build/kms-validation/mesa26-*.log` に保存しています（ビルド成果物なのでコミット対象外）。
実機のconnector列挙・TEST_ONLY・静止画・PanVK→KMS・OpenXR表示について、
以前のMesaでの成功記録は上記既存節を参照してください。今回の新MesaでのOpenXR表示と
修正後のgeometry／同期／復元の実測は、ユーザー実機での次回検証待ちです。
