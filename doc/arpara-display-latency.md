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

### 実機ログ15（hello_xr単体、設定変更なし）

ユーザー確認: hello_xr単体で取得、描画設定の変更なし。
`ArparaOpiLog/15/drm.csv` を再集計し、同梱summaryと一致した。
CSVのperiod=13,889,347nsは71.998Hz。Timewarpは4558フレームすべて有効、
fast pathは4543フレームで有効。scanout補償は0。
service起動ログ/metrics protobuf/pose診断ログは15にないため、GPU/ICD/commit/各眼実寸は
このCSV単独では再確認できない。ユーザーの追加確認により、既にscale=100%を適用済み。
以前の140%という推定は誤りだった。実際のアプリswapchain寸法は次回ログで確認する。

全4558フレームでは66.162fps、起動/終了のfast path無効15フレームを除いた
通常描画4543フレームでは66.481fps。以下は通常描画区間:

| 区間 | p50 ms | p95 ms | p99 ms | max ms |
|---|---:|---:|---:|---:|
| compositor GPU query duration | 13.793 | 13.929 | 14.026 | 20.052 |
| pose取得→KMS flip | 21.681 | 27.922 | 28.548 | 29.117 |
| GPU query終了→KMS flip | 7.563 | 13.807 | 14.370 | 14.805 |
| present要求→atomic commit | 6.437 | 12.742 | 13.398 | 13.726 |
| flip更新間隔 | 13.889 | 27.776 | 27.789 | 27.866 |

通常描画中の更新なしVBlankは377、desiredを0.5ms以上超えたflipは370/4543（8.14%）。
submit済み未flipは最大2件、clock不明のflipは0、最後までflipのない要求は0。
全区間のresource fence待ちはp50=0.022ms、queue idle待ちはp50=7.818ms。
queue idle待ちは同じGPU workの終了待ちと重なるため、13.793+7.818msと加算してはいけない。
present→commitも描画GPU workと重なっている。この集計は区間の合計でMTPを求めるものではない。

13.889msの周期に対しGPU処理がほぼ1周期を占め、p95では周期を超える。
一定のheadroom不足とVBlank待ちが提示遅れに寄与していることが観測できた。
GPU query開始→終了は実行区間だが、メモリ/キャッシュ/依存待ちも含み得る。
shader arithmetic、texture bandwidth、PanVKの効率のどれが支配的かはまだ分離できない。
前回の公式光学補正で推奨描画サイズが増えたこととの因果関係も、今回だけでは断定しない。
実測pose→flip約22msに加え、USB/サンプルの古さ、HMD内部処理/発光の遅延は未測定。
arparaが未来予測しないというソース所見は残るが、今回のCSVにpose receiptがないため
その遅延寄与を数値として分離していない。

次のA/Bは既存service起動の `XRT_COMPOSITOR_SCALE_PERCENTAGE=100` を70へ変更する。
最終出力3840×1920、71.998Hz、補正モデル/FOV、3画像、同期、場面は同一とし、
アプリを再起動して変更した推奨サイズを反映する。物理モードやviewportは変更しない。
100%なら推奨各眼2544×2544、70%なら約1780×1780になるが、実寸はアプリログで確認する。
GPU処理時間が半減すると約束するものではない。帯域負荷等の寄与を測る診断であり、
恒久的な画質変更とはしない。戻すときは100を明示する（変数を外すと既定140へ戻る）。
CSV/summary/serviceログはbaselineと別名で保存し、同じ約60秒、同じ動きで比較する。
GPU時間・pose→flip・VBlank遅れが改善するなら描画負荷への対処を優先し、
変わらなければ最終出力の補正処理や同期を次の候補とする。
pose診断は性能A/Bと分けて短時間の `ARPARA_POSE_TIMING=1` で取得する。

### 実機ログ16（scale70%、hello_xr）

ユーザーのserviceログでMonado `5e09a38c2`、Mali-G610 MC4/API1.4.354、
card0/DP-1/CRTC82/plane39、3840×1920@71.998、pixel clock573290kHz、
XRGB8888/LINEAR、3画像、sync FD→IN_FENCE_FD、最終extent3840×1920を確認。
OUT_FENCE_PTRはsupportedだが未使用。正常終了時のKMS復元がログにある。
CPU pixel copy/CPU release fence waitは0。ユーザー指定scale70%。
描画API queueはCOMPUTE。CSVでfast path有効1663フレームを集計:

| 通常描画の指標 | 15: scale100% | 16: scale70% |
|---|---:|---:|
| 更新fps | 66.481 | 66.925 |
| GPU query duration p50 ms | 13.793 | 13.817 |
| GPU query duration p95 ms | 13.929 | 14.372 |
| pose取得→flip p50 ms | 21.681 | 21.618 |
| pose取得→flip p95 ms | 27.922 | 27.927 |
| desired超過 >0.5ms | 370/4543 (8.14%) | 126/1663 (7.58%) |

100→70%で通常描画区間のGPU時間/pose→flipに大きな改善はない。
画素数の減少に比例する改善もない。試験時間/温度/周波数/sceneは完全には固定できておらず、
約0.44fpsの差を有意な改善とは扱わない。これ以上scaleを下げる優先度は下げる。
最終合成は両試験とも3840×1920で、Computeの最終補正/出力処理が候補となる。
正確なボトルネックの特定には、処理の別経路またはGPU内の段階別計測が必要。

次の一変数A/Bはscaleを100%へ戻し、`XRT_COMPOSITOR_COMPUTE=0` を指定する。
これは既存Graphics合成/ディストーションメッシュの経路。最終extent/モードを変えず、
ComputeのSTORAGE画像に対してGraphicsはCOLOR_ATTACHMENT画像を要求する。
DMA allocatorは要求usageごとにexportabilityを照会し、KMS importにもTEST_ONLYが必要。
この実機でBGRA8_SRGB/LINEARのCOLOR_ATTACHMENT直接scanoutはまだ未検証。
対応しなければ初期化エラーを保存し、Compute=1へ戻す。CPUコピーfallbackは追加しない。
同期/3画像/レンズモデル/Timewarpを変更せず、同じhello_xr場面でGPU時間とpose→flipを比較する。
既存機能の比較なので新しいビルドは不要。Graphicsが成功しても格子/左右中心/色と
画像破損の有無を確認し、同じ光学モデルでもCompute lookupとmesh補間の差を考慮する。

### 実機ログ17・18（72Hz、2026-10-11解析）

17はユーザー指定scale20%。18の提供ディレクトリにはserviceの標準出力ログはなく
`drm.csv` のみだが、ユーザーの追記した完全な起動コマンドで
`XRT_COMPOSITOR_SCALE_PERCENTAGE=100`、`XRT_COMPOSITOR_COMPUTE=0` を確認した。
18はGraphics指定、最終出力3840×1920@71.998、sync FD有効の試験として扱う。
両CSVのfast_path=1区間を同じ基準で集計した。

| 通常描画の指標 | 17: scale20% (1771 frames) | 18: Graphics、scale100% (2925 frames) |
|---|---:|---:|
| 更新fps | 66.686 | 71.999 |
| GPU query duration p50/p95/p99/max ms | 13.804/14.137/14.276/20.806 | 2.104/2.446/3.273/6.042 |
| pose取得→flip p50/p95/p99/max ms | 21.655/27.929/28.546/29.151 | 27.255/27.375/27.403/27.611 |
| GPU完了→flip p50 ms | 7.531 | 24.811 |
| present request→commit p50 ms | — | 13.168 |
| commit→flip p50 ms | — | 13.852 |
| flip間隔 p50/p95/max ms | 13.889/27.777/27.845 | 13.889/13.892/13.929 |
| 更新のないVBlank数 | 141 | 0 |
| desired超過 >0.5ms | 143/1771 | 0/2925 |

17はscale100/70の結果とほぼ同じであり、アプリ推奨解像度の追加削減を優先しない。
18はGPU処理の短縮と72Hzの安定提示を確認できたが、姿勢取得から提示までの遅延は
約27msに伸びた。MTP改善やHMD発光遅延の改善としては扱わない。

18のframe200ではpose取得を0msとしてGPU完了2.731ms、commit13.488ms、
flip27.347ms。GPUが早く終わっても、以前のflipを待ってcommitする間に姿勢が古くなる。
ソース上も `comp_drm_kms_queue_imported_fenced()` がcommit前に以前のflipをdrainする。
完了フレームで数えたsubmit済み・未flipの最大数は2。
`vkQueueWaitIdle` はp50 0.108msであり、18では全体同期CPU待ちの削除よりも
pose取得/描画開始の時刻を後ろへ寄せる方が優先候補となる。

次の最小実験は18のGraphics/scale100%設定を固定した上で、以前のflip待ちを
pose取得より前へ移す、またはVBlank位相と測定GPU時間に基づく遅いwakeを
opt-inで比較すること。3画像とqueued/scanout再利用禁止、sync FD handoffは維持する。
単に予測budgetを短くすると、現状のcommit待ちを残したまま誤った早いdisplay timeを
返す可能性があるため、予測時刻だけを変更しない。実機A/Bによるfps・deadline・
pose取得→flipと光学的な表示確認を行うまで、既定動作には採用しない。

### 次の実機A/B: pre-frame flip wait（実装済み、実機未検証）

`XRT_COMPOSITOR_DRM_PRE_FRAME_WAIT=1` を追加した。既定は0（従来動作）。
有効時はcompositor threadの `target_calc_frame_pacing()` で以前のflipをdrainし、
その後に予測、multi compositorのlayer選択、Timewarp pose取得を行う。
GPU描画後のpresent側drainも削除せず、slot再利用とFOREIGN ownershipの条件も維持する。
フレームを提示直前に予測するため計画budgetは半周期へ変更する。
これは固定の計画上の余裕であり、実測GPU時間や発光時刻として扱わない。
開始時刻はdrain完了後すぐで、VBlank直前までさらに遅らせる変更は今回は行わない。

Graphics（COMPUTE=0）とexport可能なsync FD handoffが必要。
KMS handoffが使えなければ初期化失敗とし、CPU fence待ちへ自動fallbackしない。
drain失敗時はtargetをfailedとし、以後のrender/acquireを拒否する。
既存のtimeoutとhotplug/master loss処理を使用し、Vulkan queue lockを保持して待たない。
3画像、IN_FENCE_FD、GPU全体同期、HID/IMU/光学補正を変更していない。

まずOrange Pi側で全体をビルドし、同じbinaryでPRE_FRAME_WAIT=0/1を比較する。
それぞれhello_xrを再起動し、同じ場面・動きで約60秒測定する。
18と同じ72Hz/scale100/Graphicsを使用する。有効側の起動例:

```bash
cmake --build build --parallel 2
XRT_COMPOSITOR_DRM_PRE_FRAME_WAIT=1 \
XRT_COMPOSITOR_DRM_TRACE_FILE=/tmp/drm-pre-frame.csv \
XRT_COMPOSITOR_SCALE_PERCENTAGE=100 XRT_COMPOSITOR_COMPUTE=0 \
XRT_TRACKING_ORIGIN_OFFSET_Y=1.6 XRT_COMPOSITOR_DRM_SYNC_FD=1 \
XRT_COMPOSITOR_FORCE_DRM=1 XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 XRT_COMPOSITOR_DRM_MODE=3840x1920@71.998 \
XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
./build/src/xrt/targets/service/monado-service 2>&1 | tee -i /tmp/monado-pre-frame.log
```

従来側はPRE_FRAME_WAITを0とし、CSV/ログ名を別にする。
正常停止後、`python3 scripts/analyze-drm-latency.py /tmp/drm-pre-frame.csv` で集計。
新しい `pre_frame_wait` はpose取得より前の待ちであり、pose→flipやGPU時間と合算しない。
期待する変化はpose→flip約27msから約1周期分の短縮、present→commit待ちの減少。
これは実測結果ではない。fps/未更新VBlank/desired誤差/p95/p99と画面破損を比較する。
アプリに伝える予測時刻も変わるため、アプリ描画遅れや古いlayerの再利用増加にも注意する。
悪化した場合はPRE_FRAME_WAIT=0へ戻す。72Hzで確認後に90Hzを別測定する。

今回のホスト全体ビルド成功、CTest 36/36成功（予測deadlineケースとpre-frame待ち集計を追加）。
`monado-cli test` は終了0だが、ホスト上はSimulated HMDでありarpara実機回帰の証明ではない。
検証ログは `build/arpara-validation/pre-frame-{build,tests,cli}.log`。
ARM64ビルド、PanVK/KMS実機表示、MTP・主観的追従感の改善は未検証。

### 実機ログ19: pre-frame waitによる姿勢の新しさ改善

`19/monado-pre-frame.log` と `19/drm-pre-frame.csv` を解析。
Monado `a8c835a9c`、GRAPHICS queue、Mali-G610 MC4/API1.4.354、
3840×1920@71.998、sync FD→IN_FENCE_FD、3画像、pre-frame wait有効を起動ログで確認。
CPU pixel copy/CPU release fence wait=0、正常終了/KMS state復元あり。
18と同じfast_path=1基準で開始/終了の空描画を除き、3694フレームを集計した。

| 指標 | 18: 従来Graphics (2925 frames) | 19: pre-frame wait (3694 frames) |
|---|---:|---:|
| 更新fps | 71.999 | 71.979 |
| GPU query duration p50/p95/p99 ms | 2.104/2.446/3.273 | 2.112/2.624/3.191 |
| pose取得→flip p50/p95/p99 ms | 27.255/27.375/27.403 | 13.650/13.807/13.816 |
| pose取得→flip最大 ms | 27.611 | 27.520 |
| GPU完了→flip p50 ms | 24.811 | 11.214 |
| present request→commit p50 ms | 13.168 | 0.120 |
| submit済み・未flip最大フレーム数 | 2 | 1 |
| flip間隔 p50/p95/p99/max ms | 13.889/13.892/13.899/13.929 | 13.889/13.893/13.901/27.779 |
| 更新のないVBlank / desired超過>0.5ms | 0/0 | 1/1 |

姿勢取得→flip中央値が13.605ms（49.9%）短縮し、ほぼ1周期分新しい姿勢を提示できた。
GPU描画速度はほぼ同じで、present後の旧flip待ちがpose取得前へ移ったことと整合する。
pre-frame待ち中央値10.861msはpose取得前であり、上の遅延に足し戻さない。
queue idle待ちは0.108→2.346msへ増えたが、GPU完了までの待ちがこの位置へ移ったためで、
pose取得→flip短縮を否定する追加遅延ではない。GPU時間との合算もしない。

唯一の遅れはframe6（通常描画2フレーム目）。GPU query duration12.848ms、
pose→GPU完了13.135msでVBlank付近となり、次のVBlankに提示された。
ログの最初の120フレーム窓の後は未更新VBlankが0で72Hzを維持。
開始直後のGPU負荷スパイクの原因は未特定であり、除外して最大値を小さく報告しない。
画像破損/tearing/光学的妥当性はCSVでは判定できないが、ログには同期/commitエラーなし。

ユーザーの「少し良くなった」という主観と測定したソフトウェア区間の改善は整合する。
ただし絶対MTP、IMUサンプルからpose取得まで、アプリlayer自体の新しさ、HMD発光は未測定。
この結果を絶対MTPが13.65msになったという意味にはしない。
現時点ではGraphics/scale100/pre-frame wait有効の72Hz設定を比較基準として保持し、
次は同じ設定のまま90.001Hzを別ログで検証する。さらに遅いwakeはその後の候補であり、
残る約11msのGPU完了→flipを全て安全に削減できると約束しない。

### 実機ログ20: 90Hz pre-frame wait

Monado `a8c835a9c`、GRAPHICS、pre-frame wait有効、3840×1920最終出力を起動ログで確認。
fast_path=1の4584フレーム: 更新90.002fps、更新のないVBlank0、未flip要求0、
submit済み未表示最大1フレーム。pose取得→flipはp50/p95/p99/maxが
10.858/10.958/10.993/22.007ms、GPU query durationは2.206/2.678/3.012/7.943ms。
submit→GPU完了はp99 3.185ms。GPU完了→flip中央値8.400ms、
present request→commit中央値0.124ms。flip間隔p50/p95/p99/maxは
11.111/11.114/11.152/11.184ms。desired超過>0.5msは1回だが、
VBlank sequenceに未更新はない。予測deadline超過と実際のscanout更新欠落を同一視しない。
正常終了/KMS復元を確認、同期/commitエラーなし。実発光/絶対MTPは引き続き未測定。

### 次の実験: late wake（実装済み、実機未検証）

`XRT_COMPOSITOR_DRM_WAKE_BUDGET_US` を追加した。既定/0は即時wakeで20と同じ動作。
非zeroにはPRE_FRAME_WAIT=1、Graphics、sync FDが必要。
値は1000us以上かつ選択mode周期未満の整数とし、不正設定は明示的に失敗する。
例の6000は「desired flipの6ms前にwake」の意味で、6msの追加待ちではない。

pre-frame drain後にKMS位相からdesired flipを計画し、その6ms前をwakeとして返す。
既存multi compositorの `wait_frame()` / `u_wait_until()` が実際に待つ。
予測時刻は先にclientへ伝え、wake後にlayer選択・Timewarp pose取得を行う。
Vulkan/DRM lockを保持したsleepや、GPU描画済み画像へのpose更新は行わない。
待ちが遅れた場合は過去のwakeを返さず、予測時点で余裕不足なら次VBlankへ繰り越す。
render完了の保証は引き続きsync FD/IN_FENCE_FDで行う。GPUが遅れた場合、
描画途中の画像を表示して短縮したことにはせず、次のflipとdeadline超過を測定する。

20ではsubmit→GPU完了p99が3.185msなので、まず6msのCPU/queue/GPU全体の
固定計画余裕で比較する。最大8.060msの例もあるため全フレームのdeadlineを保証しない。
adaptiveなGPU時間予測、固定4msのdisplay offset、IMU予測、Timewarpモデルは変更しない。
狙いは姿勢取得→flip約10.9msを約6msへ縮めることだが、実機の結果で確認する。

同じbinaryでWAKE_BUDGET_US=0/6000を比較する。各回hello_xrを再起動し、
90.001Hz、scale100%、同じscene/動きで約60秒測定する。有効側:

```bash
cmake --build build --parallel 2
XRT_COMPOSITOR_DRM_WAKE_BUDGET_US=6000 \
XRT_COMPOSITOR_DRM_PRE_FRAME_WAIT=1 \
XRT_COMPOSITOR_DRM_TRACE_FILE=/tmp/drm-late-wake.csv \
XRT_COMPOSITOR_SCALE_PERCENTAGE=100 XRT_COMPOSITOR_COMPUTE=0 \
XRT_TRACKING_ORIGIN_OFFSET_Y=1.6 XRT_COMPOSITOR_DRM_SYNC_FD=1 \
XRT_COMPOSITOR_FORCE_DRM=1 XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 XRT_COMPOSITOR_DRM_MODE=3840x1920@90.001 \
XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
./build/src/xrt/targets/service/monado-service 2>&1 | tee -i /tmp/monado-late-wake.log
```

CSVにはplanned_wakeを追加し、集計はwake_minus_planned（実際のwake遅れ）と
wake_to_pose_fetchを分離する。予定wakeを実測wakeとして使わない。
fps/未更新VBlank/desired誤差/pose→flipのp95/p99/maxを比較し、悪化時は
WAKE_BUDGET_US=0へ戻す（PRE_FRAME_WAIT=1は維持）。CPU pixel copy/release fence待ち0、
3画像の寿命、ownership transfer、光学補正、HID/IMUは変更していない。
実機の画像破損・追従感・WiVRnでの回帰は別途確認する。

late wake変更のホスト全体ビルド成功、CTest 36/36成功。
deadline/wakeの境界ケースと予定wake・実測wake・pose取得の分離を自動テストで確認。
検証ログは `build/arpara-validation/late-wake-build.log` と `late-wake-tests.log`。
今回変更のARM64ビルド、PanVK/KMS実機表示・遅延改善は未検証。

### 実機ログ21: late wake 6ms

fast_path=1の5917フレーム: 更新89.987fps、更新のないVBlank1、
desired超過>0.5ms 2、submit済み未表示最大1、未flip要求0。
pose取得→flip p50/p95/p99/max=5.781/5.878/5.936/17.042ms。
20の中央値10.858msから5.077ms（46.8%）短縮した。
GPU duration p50/p95/p99/max=2.223/2.921/3.135/6.888ms、
submit→GPU完了p99=3.320ms、GPU完了→flip中央値3.240ms。
実際のwake遅れはp50/p95/p99/max=0.015/0.067/0.117/0.938ms、
wake→pose取得中央値0.198ms。同期エラーなし、正常終了/KMS復元あり。
表示までの余裕をさらに削るとGPUの長いtailやwake jitterでVBlankを逃し得るため、
まず6msの設定を固定して以下の姿勢予測を別の一変数A/Bとして評価する。
ソフトウェア区間の改善であり、絶対MTP・物理的光学応答の実測ではない。

### Monado relation historyによるarpara姿勢予測（実機未検証）

従来arparaは `at_timestamp_ns` を無視して最新の融合姿勢を返していた。
compositorのTimewarpは有効で、desired flip+固定4msを姿勢要求時刻として使っているが、
arpara側に角速度を用いた未来外挿はなかった。固定4msはpacer由来の推定であり、
arparaの発光/scanout測定に由来しない。今回もこの値を無根拠に変更しない。

ユーザー指定に従い独自外挿式を追加せず、Monadoの `m_relation_history` を使用する。
受信ごとの融合姿勢とbias補正済みジャイロをrelationに保存する。
body座標のrad/sを融合quaternionでtracking/world座標へ変換してangular_velocityに設定。
`m_relation_history_get()` が過去を補間し、未来を `m_predict_relation()` によって外挿する。
この既存関数がworld角速度をbodyへ戻して姿勢積分するため、二重回転/軸順序誤りを避ける。
予測結果を履歴やfusionへ再投入せず、各要求は同じ未予測のIMU履歴から計算する。

service側 `ARPARA_POSE_PREDICTION_MS=10` で有効化。値は0〜20の整数、既定0。
指定値は外挿の最大時間であり、毎回固定の時間を追加する値ではない。
requested timestampと最新履歴時刻の差を上限で制限して照会する。
過去20ms内の照会にはhistoryの補間/完全一致を使い、最古以前へのreverse predictionは採用しない。
切断/追跡準備前/host受信から20ms超の古いサンプルは外挿しない。
counter再seed時には履歴をclearし、タイミング不連続をまたいで補間しない。
positionは固定原点のvalidのみでPOSITION_TRACKEDを付けず、3DoFを維持する。

fusion間隔は引き続きdevice counterを使用する。履歴照会の基準時刻はhost monotonicの
受信時刻とし、未同期の積算fusion_nsをhost時刻と直接比較しない。
これは同期されたIMU capture timeではなくUSB/HID受信のproxyで、輸送遅延や
gyro mounting/bias、IMU-to-eye変換、HMD発光の未校正分は残る。
要求時刻を尊重した予測であっても、正しい絶対MTPや完全な発光同期は未実証。

21の起動コマンドに `ARPARA_POSE_PREDICTION_MS=10` だけを追加し、
WAKE_BUDGET_US=6000、90.001Hz、Graphics、scale100、同じsceneを維持して約60秒比較する。
CSV/ログを別名で保存し、予測0への切り戻しと比較する。
GPU/fpsを変える最適化ではなく、回転中に表示される姿勢を新しくする比較。
頭を回す/止める/反転する際のovershootや静止時の揺れ、左右の見え方を確認する。
`ARPARA_POSE_TIMING=1` は別の短い診断runで使用し、要求/受信/query/fusion時刻に加え
prediction_ns、history_resultを記録する（PREDICTED=3、INTERPOLATED=2、EXACT=1）。
Python集計の `--pose-log` はapplied_prediction分布も出力する。
大量ログの有無を変えた結果を同じ負荷の性能A/Bとして混ぜない。

予測の上限/鮮度、rad/sの角度、非identity姿勢のbody/world変換、historyの
EXACT/INTERPOLATED/PREDICTED、ゼロ角速度、位置追跡を偽装しないflagsを自動テスト。
予測有効の模擬HID起動・未来姿勢取得・切断もCTestに追加。
ホスト全体ビルド・CTest 37/37成功。ARM64ビルド・実機予測の追従改善は未検証。
検証ログは `build/arpara-validation/pose-prediction-{build,tests}.log`。
KMS、DMA-BUF、sync FD、3画像、6ms wake、光学補正、HID protocolは変更しない。

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
