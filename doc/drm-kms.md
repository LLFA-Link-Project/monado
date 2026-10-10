<!-- Copyright 2026, OpenAI. SPDX-License-Identifier: BSL-1.0 -->
# Linux DRM/KMS ネイティブ出力（実験的）

`XRT_HAVE_DRM_KMS` は Linux、libdrm、Vulkan が利用可能な場合の条件付きオプションです。
既存の arpara Rust protocol/HID/IMU ドライバは変更しません。
`drm_kms` factory は明示指定専用で、自動選択しません。
VkDisplayKHR / VkSurfaceKHR / VkSwapchainKHR、X11、Wayland を使わず、peek window も生成しません。

この変更では診断用 CPU コピー経路を実装しています。

```
Monado renderer -> BGRA8 optimal VkImage (GENERAL)
 -> transfer submission (render_complete binary semaphore を一度消費)
 -> staging buffer -> fence wait -> 非 coherent memory の invalidate
 -> XRGB8888 linear DRM dumb buffer（実際の pitch に沿って行コピー）
 -> atomic commit -> page-flip event 完了 -> 前の scanout buffer を再利用
```

Vulkan 画像と KMS buffer はそれぞれ 2 枚です。描画と転送は同一 queue/family を使用し、
queue mutex で外部同期します。転送と page-flip の待ちは各約 3 秒を上限とし、
失敗したフレーム以降は buffer を再利用しません。GPU 完了待ちと全画面 CPU 転送を含むため、
ゼロコピー・低遅延実装ではありません。fake pacer の周期には選択した実 KMS refresh を使います。
精密な vblank 時刻への pacing、desired-present-time scheduling、sync FD は未実装です。

## Orange Pi 5 でのビルド

リポジトリの submodule は従来通り Rust protocol 実装を参照します。

```bash
git submodule update --init --recursive
# libdrm と Vulkan の開発ヘッダー/ライブラリ、従来の Monado/Rust 依存が必要。
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
modifier の KMS との共通集合によるゼロコピー接続はまだ実装していません。

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

## この作業環境での検証範囲

- x86_64 の既存 `build/`: 全体ビルド成功。
- arpara 回帰 CTest: 3/3 成功。CLI は実機がないため Simulated HMD（従来の未接続挙動）。
- SwiftShader: 64x64 readback 全画素と semaphore reuse の検証成功。PanVK の検証ではありません。
- AArch64 GCC 14.3: 新規 3 module と変更した compositor/renderer/settings、probe の object compile 成功。
  headless 用ヘッダー構成で確認。ARM ライブラリとの link / Orange Pi 実行は未検証です。
- `/dev/dri` が存在しないため、DRM 列挙/TEST_ONLY/静止画/PanVK→KMS/OpenXR 表示は未検証。
  probe は失敗理由を出して終了します。サービスは compositor 生成失敗を報告します。
  既存の service mainloop が残る場合は SIGINT で終了します（今回も SIGINT で終了を確認）。

Vulkan 初期化前の起動失敗で未初期化 queue mutex を破棄して abort する既存問題も修正し、
GPU 不要の lifecycle 回帰テストを追加しています。arpara と合わせて CTest 4/4 成功。

詳細ログは作業ツリーの `build/kms-validation/` に保存します（Git 管理対象外）。
FOV、IPD、レンズ歪み、IMU 軸/校正は従来の暫定状態を維持します。
