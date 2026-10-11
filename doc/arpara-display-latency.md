# arpara / Orange Pi 5 表示遅延調査（2026-10-11）

## 状態と保存したbaseline

調査開始時のMonadoは `f2222d155`、作業ツリーはclean。
参照ブランチ `baseline/arpara-latency-f2222d155` を作成した。
今回の変更は計測とMetrics整合性の修正。低遅延化の実機A/B結果はまだない。
頭部回転遅延が改善したとは報告しない。

作業ホストはx86_64、`Linux archlinux-v 7.2.9-arch1-1`。
`/dev/dri` がなく、`monado-kms-probe --list` は
`No accessible KMS card`。実機のカーネル/Mesa/ICD確認、表示、発光測定は実行不能。
Orange Pi側の既存情報はRK3588/Mali-G610、ユーザー報告のMesa 26.1.6/PanVK。
以前の実機報告のカーネルは `6.18.55-current-rockchip64`。
これらは今回の取得値ではない。実機で下記の環境情報を再取得する。
カーネル、OS設定、WiVRn、エンコーダ/デコーダは変更していない。

## 表示経路と調査所見

このcheckoutの `XRT_COMPOSITOR_FORCE_DRM=1` は独自 `drm_kms` targetを選択する。
Vulkan描画 → BGRA8/LINEAR DMA-BUF → Rockchip GEM/FB → atomic KMS → DP出力。
Wayland/X11、VkSurfaceKHR、VkSwapchainKHR、vkQueuePresentKHR、VkDisplayKHRを使わない。
Vulkan Present Modeはこの経路には該当しない。3画像、最大1件のqueued atomic commit。
GPU/表示中画像の早期再利用防止、FOREIGN ownership、sync FDハンドオフは維持する。
`XRT_COMPOSITOR_PREFERRED_IMAGE_COUNT` はWSI swapchain用で、このDRMの3画像には効かない。
今回、2画像化やtearing/async flipを導入しない。

以前の実機ログではcard0/DP-1、connector84/CRTC82/plane39、3840×1920、
71.998Hzまたは90.001Hz、pitch15360、XRGB8888/LINEAR、GPU0 PanVK。
IDは再列挙して確認し、別環境では固定値として扱わない。
target生成時のログにpixel clock、horizontal/vertical totals、OUT_FENCE_PTRの有無も追加した。
OUT_FENCE_PTRは照会のみで使わない。IN_FENCE_FD/検証済みreservation同期を維持する。
毎フレームのatomic requestはconnector/CRTC/planeのプロパティを再設定し、ALLOW_MODESETを付ける。
実際に毎回modesetしたことを意味しない。後続の候補は初回以降FB_ID/fenceだけの更新だが、
commit時間が支配的か測定してからTEST_ONLYとA/Bを行う。

`comp_renderer_draw()` はpresentの後、共有render/scratch/commandリソースの解放前に
毎フレーム `vkQueueWaitIdle` を呼ぶ。次のフレーム準備にはrender fence待ちもある。
単純な待機削除は安全でない。待機が支配的なら、render fenceで保護できる範囲を確認し、
mirror/peek/その他queue workまで含む全体待ちを局所化するか、フレーム別リソースにする。
3画像と「CPU release fence waits=0」だけでは、renderer全体のCPU待ちがゼロとはいえない。

DRM予測は直近flip位相と、sync FD利用時には1周期の固定予算を使う。
fake pacerのpresent-to-display offset（既定4ms）も残る。これは発光の実測値ではない。
従来はfake pacerの予測を返した後にDRM側で時刻を上書きしており、Metricsのsystem_frameと
実際のTimewarp対象時刻が不一致だった。今回、上書きを記録前へ移し、実際の予測を保存する。
KMS flipにはフレームIDを付け、monotonic eventが利用可能な場合だけsystem_present_infoを記録する。
未知のearliest_present/present_margin/GPU完了予定などは0のまま（未計測）で埋め合わせない。

arpara `get_pose()` は `at_timestamp_ns` を使わず、最新融合姿勢を返す。
角速度による未来予測はない。Timewarpは `!c->debug.atw_off` で有効だが、未来の表示時刻を
要求してもこのドライバから返るのは最新受信サンプルの姿勢。
表示予定までの姿勢の古さが残り得るが、その寄与の大きさはまだ測定していない。
今回はgyro軸・バイアス・予測量を変更しない。
counter差分でfusion間隔を積算し、初回/100ms以上の受信停止時にはタイミングを再初期化する。
fusion時刻はホスト時刻に一度anchorした積算値で、取得時刻の精密なclock同期ではない。
通常の16bit wrapは既存Rust差分処理、長時間のdrift/USB batchingは今後実測する。
scanout compensationは既存のdevice compositor infoに従う。HMDの物理走査/発光方式は未確認。
今回のCSVには適用されたscanout補償時間、Timewarp有効/無効、fast pathも記録する。

## 計測点と限界

既存 `XRT_METRICS_FILE` をservice側で使う。session_frame/used/system_frameを合わせて
application session frame IDとsystem frame IDを対応させる。
sessionのwhen_wait_woke/when_deliveredはサーバ側の観測であり、クライアント関数の
厳密な復帰/呼び出し瞬間ではない。厳密なT0/T1/T2にはOpenXR API traceまたは
hello_xr側の計測が別途必要。今回はアプリ/WiVRnを変更していない。

`XRT_COMPOSITOR_DRM_TRACE_FILE=/tmp/drm.csv` で追加CSVを有効にする。
列はevent,frame_id,monotonic_ns,value。既定は無効。buffered fileで、正常終了時にcloseする。
異常終了では末尾が欠落し得る。計測自体のCPU/I/O負荷も比較し、長時間常用しない。

| 時点 | CSV/既存Metrics | 解釈 |
|---|---|---|
| T3 | begin | compositor描画フレームの取得 |
| T4 | pose_request/pose_return | Timewarp用pose呼び出し。valueは対象時刻 |
| T5 | submit_begin/submit_end | CPU submit前後。GPU完了ではない |
| T6 | gpu_start/gpu_end | calibrated timestampsが利用できる場合だけhost clockへ変換 |
| T6補助 | gpu_duration | timestamp query対応時のcompositor GPU処理時間。絶対時刻の代用不可 |
| T7 | present_request / commit_begin/commit_end | release要求と、前flip待ち後のatomic ioctlを分離 |
| T8 | flip | monotonic KMS page-flip event。発光時刻ではない |
| T8未対応 | flip_receipt | event clock不明時のホスト受信時刻。実提示時刻の集計から除外 |
| T9 | display_estimate | 推定値。desiredとの差はpacer offset |
| T10 | なし | フォトダイオード/高速度カメラが必要 |

resource_wait_begin/end、queue_idle_begin/endでCPUブロック区間を区別する。
GPU queryはrender command区間であり、release ownership barrierやアプリ描画全体ではない。
GPU絶対時刻の変換ができなければsubmit→GPU完了、GPU完了→flipはnullと出す。
flip→display_estimateの差は「page flipと推定発光時刻の差」であり、発光予測誤差の実測ではない。

`ARPARA_POSE_TIMING=1` は要求時刻、ホスト受信時刻、問い合わせ時刻、fusion積算時刻、
返したquaternionを出す。フレームごとのJSON解析やIMU計算変更はない。
poseログは別の短時間診断に使い、通常の性能測定では無効にする。
requested時刻とCSV pose呼び出し区間で照合できるが、アプリframe IDへの直接リンクではない。
host receipt ageはUSB/デバイス内遅延を含まない。実IMU取得時刻の古さとは区別する。

集計:

```sh
python3 scripts/analyze-drm-latency.py /tmp/drm.csv > /tmp/drm-summary.json
python3 scripts/analyze-drm-latency.py /tmp/drm.csv --html /tmp/drm-timeline.html
# pose診断を行った場合だけ
python3 scripts/analyze-drm-latency.py /tmp/drm.csv --pose-log /tmp/monado-local.log
```

各区間のmean/p50/p95/p99/max、更新FPS、sequence gap、desired deadline超過（0.5ms許容）、
submit済みでflip未完了の最大件数を出す。未完了末尾フレームは件数の算出から除外する。
sequence gapは新FBへ更新しなかったVBlankであり、アプリフレーム欠落とは同義でない。

## 実機の再現手順

baseline commitは別worktreeでビルドできる。現在のbuild/cacheを削除しない。
既存serviceの停止後、同じローカルVT/seat権限で順に実行する。
権限変更、sudo service起動、カーネル差し替えは不要。DRM master競合/ACL失敗はログを残す。

```sh
git rev-parse HEAD
git status --short
uname -a
vulkaninfo --summary
pkg-config --modversion libdrm
./build/src/xrt/targets/kms_probe/monado-kms-probe --list
./build/src/xrt/targets/kms_probe/monado-kms-probe --vulkan-list
cmake --build build --parallel 2
```

ICDはserviceの有効device extensions/GPUログと、必要なら `VK_LOADER_DEBUG=driver` で確認する。
開始時のmode/plane/GPUログ、アプリの各眼描画サイズ、CPU/GPU負荷、温度を両試験で保存する。
まず同じhello_xr、同じscale、3840×1920@71.998、同じ場面でbaselineを60秒測る。
90Hzは別比較として扱い、72Hzと混ぜて改善率を算出しない。

```sh
XRT_COMPOSITOR_FORCE_DRM=1 \
XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 \
XRT_COMPOSITOR_DRM_MODE=3840x1920@71.998 \
XRT_COMPOSITOR_DRM_SYNC_FD=1 \
XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
XRT_COMPOSITOR_LOG=info \
XRT_METRICS_FILE=/tmp/monado-local.protobuf \
XRT_COMPOSITOR_DRM_TRACE_FILE=/tmp/drm.csv \
./build/src/xrt/targets/service/monado-service 2>&1 | tee -i /tmp/monado-local.log
```

別端末で既存 `hello_xr -G Vulkan` を起動し、同じ頭部回転を行う。
hello_xr終了後、serviceをCtrl-Cで停止してCSV/metricsをflushする。
次は同じ条件でtrace無効も比較し、計測負荷を確認する。
短いpose診断では上のservice起動に `ARPARA_POSE_TIMING=1` を追加する。
90Hzではmodeだけ `3840x1920@90.001` に変え、ログのファイル名も変更する。
現在のDRM経路は既にdisplay serverを迂回するため、VK_DISPLAYへの変更は最初の対策ではない。
デスクトップ経由の比較は必要な環境が用意できた場合だけ別試験として行う。

## 比較結果と次の最小変更

過去ログ `ArparaOpiLog/3/monado-drm-90hz.log`（Monado `608615e4c`）の19窓、
2280 intervalsから集計した更新FPSは約86.548、更新のないVBlankは91。
古いMesa/仮の光学設定の記録で、現在の公式光学補正ビルドのbaselineには流用できない。
GPU完了/姿勢時刻/発光時刻のログがなく、これだけではMTPを算出できない。

現時点の比較表:

| 項目 | baseline f2222d155 | 今回の計測パッチ |
|---|---|---|
| hello_xr実機追従感・映像正常性 | ユーザーの遅延申告 | 未検証 |
| GPU time / submit / presentation p95,p99,max | 現条件のログなし | 収集可能、実測待ち |
| 滞留/予定VBlank遅れ/poseの古さ | 未測定 | 収集可能、実測待ち |
| 絶対Motion-to-Photon | 外部測定なし | 外部測定なし |

GPU時間やrender負荷が支配的なら、校正後に増えたアプリ推奨解像度も同一条件で確認する。
queue idleが支配的なら安全な局所fence/リソース分離を一つずつ検証する。
GPUが早く終わるのにcommit/flipまで1周期以上待つなら、位相予測・FIFO/backpressureを検証する。
poseのrequested horizonが大きく、受信姿勢をそのまま表示している寄与が確認できたら、
gyro座標系・角速度・時刻同期を検証した短時間予測を独立A/Bにする。
固定過大予測、同期削除、無検証のscanline設定は行わない。
単体KMS色切り替えとMonadoの外部発光測定で、残るHMD内部処理遅延を切り分ける。
ローカルhello_xrの改善を説明できてからWiVRnを再評価する。

## 検証記録

既存buildの全体ビルドとCTestを実行する。KMS fault injectionではcommitの成功/失敗、
sync FD所有権、queued/scanout再利用防止に加え、observerのimage対応/monotonic判定を確認する。
pacerテストは上書きした予測が1回だけ適用されることを確認する。
Python集計テストは未測定clockをnullにすること、sequence wrap、符号付き予測差、
percentile、pose receipt ageを確認する。
ホストの全体ビルド成功、CTest 36/36成功（追加pacingケース、KMS fault injection、Python集計を含む）。
ログは `build/arpara-validation/latency-build.log`, `latency-tests.log`, `latency-kms.log` に保存した。
ARMクロスビルドは旧cacheの `/tmp/arpara-arm-tools/...` が消失しており実行不能。
前回のARM成功を今回の成功として流用しない。Orange Pi側の全体ビルドが必要。

参照:
[Monado Metrics](https://monado.pages.freedesktop.org/monado/metrics.html)、
[Frame Pacing](https://monado.pages.freedesktop.org/monado/frame-pacing.html)、
[DRM UAPI](https://docs.kernel.org/gpu/drm-uapi.html)。
公開資料とこのcheckoutを照合して使用可能な機能だけを扱った。
