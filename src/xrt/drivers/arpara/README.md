# arpara 5K driver（実験段階）

USB `2c30:1050` のみを legacy Prober の `found` で処理し、Monado が選んだ HID interface 0 を
`xrt_prober_open_hid_interface()` で開く。ノード名の固定や別の USB 探索はしない。
名称は `arpara 5K`、2 views、入力は `XRT_INPUT_GENERIC_HEAD_POSE`。

## Rust 実装の利用

`src/external/vrg9080-protocol-rs` は、ユーザーの `~/vrg9080-protocol-rs` の
commit `0b8a8ca23cd0e74c282b8d0fb43f2a0d6caa4664` を取り込んだ submodule。
URL は https://github.com/ReinaS-64892/vrg9080-protocol-rs.git 。

`rust/src/lib.rs` は、この submodule の `src/codec.rs` と
`src/transport/protocol.rs` をそのまま Rust モジュールとしてコンパイルする。
後者は upstream で private なため、固定した revision のソースを直接参照している。
受信 parser、checksum 検査、15段階の固定 startup report、status ACK を C++ で再実装しない。
Cargo はネットワーク不要の `--offline --locked` で `libmonado_arpara_bridge.a` を生成し、
C++ の `arpara_hmd.cpp` が C ABI を介して呼び出す。
upstream の Linux `DeviceSession` は独自にノードを探索・オープンするので使用せず、
Monado の HID 抽象化を唯一の接続とする。upstream submodule 自体は変更していない。
Rust ソースと bridge のライセンスは GPL-3.0-or-later。

Rust/Cargo 1.85 以上が必要。Linux/aarch64 でネイティブにビルドできる。
クロスビルドの場合は対応する Rust target と linker/toolchain を用意し、
`-DARPARA_RUST_TARGET=aarch64-unknown-linux-gnu` などを明示する。

```sh
git submodule update --init src/external/vrg9080-protocol-rs
cmake --build build --parallel 2
ctest --test-dir build -R '^tests_arpara' --output-on-failure
```

`XRT_BUILD_DRIVER_ARPARA` でドライバの有効・無効を切り替えられる。
Cargo がない環境では自動検出で有効化しない。

## 挙動と制限

- HMD を直ちに登録し、専用スレッドで固定 startup を順に送信する。
  power mode/state とサンプリング設定は一時的なデバイス状態を変更する。
  command 応答 status を検査し、1秒（0x8D のみ3.5秒）の期限を設ける。
  失敗すると後続の初期化 command を送らず、受信済み／以後の IMU の処理は継続する。
- 応答待ちにも IMU の姿勢更新と status ACK を処理する。未検証 command、
  firmware 更新、EEPROM 書換えは送らない。送信は report-ID slot を含む65バイト。
- Rust codec で64バイトの長さ・version・checksum・9 float の有限性と範囲を検査する。
  gyro は deg/s → rad/s、accel は g → m/s² に変換し、Monado の `m_imu_3dof` を使う。
- 16-bit の10kHz counter 差を fusion に使用する。wrap を扱い、重複は捨て、
  15ms 超の差または100ms超のホスト受信停止は時間基準を再設定する。
  最新姿勢を mutex 保護で返す。要求時刻への予測は行わない。
  ホスト単調時刻で鮮度を判断し、100ms無受信・切断で orientation tracked を外す。
- 軸変換は暫定の identity。HMD の取り付け軸、符号、左右の回転方向は未検証。
  `sensor_vectors()` を実機測定に基づいて修正する必要がある。
  磁気値は検査のみで yaw 補正には使わず、yaw drift があり得る。
  position tracking は宣言しない。生の位置はゼロで、POSITION_VALID を付け、POSITION_TRACKED は付けない。
  床基準の高さはサービス側の既存 `XRT_TRACKING_ORIGIN_OFFSET_Y=1.6` で設定できる。
- IMU が届くまでは identity orientation を返し、tracked を付けない。
  `ARPARA_IMU=false` は送信も読取スレッドも使わない固定姿勢の登録確認モード。
- 登録用の画面geometryは暫定2560×1440/60Hz、130×73mm、レンズ間隔65mm。
  DRMでは選択された物理モードと出力viewportを使用する。これはパネルの真のnative解像度を確定しない。
  光学補正・FOVは下記の公式JSON由来のPOLY3を使用する。IPDは呼出し元のdefault eye relationを維持する。
- destructor はスレッド停止と join の後に HID・fusion・mutex を解放する。
  切断後の自動再接続、Lighthouse 6DoF、Direct Mode 出力は未実装。

## 検証と実機での次の手順

`tests_arpara` は模擬 HID と実際の Rust parser／Monado fusion を使い、
既知送信ベクタ、破損・短縮・誤version・非有限値、wrap、startup 応答との混在、
ACK、姿勢更新、head role 割当、製品IDの限定、open失敗、応答タイムアウト、応答待ち中の終了、切断と解放を検査する。
`tests_arpara_fixed` は IMU 無効時に送信がないこと、`tests_arpara_rust` は Rust FFI を検査する。
模擬通信の成功は実機の姿勢追跡成功を意味しない。

今回の作業環境は x86_64 で、初期状態には `build/` と `/dev/hidraw*` がなかった。
`cmake -S . -B build -DBUILD_TESTING=ON` で作成した build を使って検証した。
実際の `monado-cli test` は `head: Simulated HMD` を表示して正常終了する。
arpara の実機登録・HID startup・センサー軸の確認と映像出力は未確認。
検証ログは `build/arpara-validation/` に保存する。

HMD 接続先で、他の HID reader を停止してから、まず送信なしで登録を確認する：

```sh
ARPARA_IMU=false ./build/src/xrt/targets/cli/monado-cli test
```

`head: arpara 5K` を確認した後、IMU ありの通常モード：

```sh
./build/src/xrt/targets/cli/monado-cli test
```

CLI の test は生成直後に終了するため、これだけで持続的な3DoF追跡や回転方向を
検証したとは扱わない。長時間動作する Monado の接続先で姿勢を実測する必要がある。
HID open が権限不足の場合は、管理者が対象 VID/PID に限定した udev の
`SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2c30", ATTRS{idProduct}=="1050", TAG+="uaccess"`
設定を検討する。今回システムのルール・パーミッションは変更していない。

## Orange Pi 5（Linux/aarch64）でのビルド

64-bit aarch64 の OS と、その OS 向けの C/C++ コンパイラ・Monado の依存ライブラリを使う。
`uname -m` は `aarch64`、`rustc -vV` の host は `aarch64-unknown-linux-gnu`
（glibc の場合）になる。32-bit armhf のビルドは今回の検証対象外。
Rust/Cargo は1.85以上、CMake は3.22以上、C++20対応コンパイラが必要。
既存の build 設定は保持し、次の順で実行する：

```sh
git submodule update --init src/external/vrg9080-protocol-rs
cmake -S . -B build -DXRT_BUILD_DRIVER_ARPARA=ON -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build -R '^tests_arpara' --output-on-failure
ARPARA_IMU=false ./build/src/xrt/targets/cli/monado-cli test
```

通常のネイティブ aarch64 ビルドでは `ARPARA_RUST_TARGET` の指定は不要。
x86_64 側の build ディレクトリや生成済み archive をコピーせず、ソースと submodule から
その環境の build を使用する。HMD 接続時の `head: arpara 5K` を確認してから IMU を有効化する。

クロスビルドでは CMake の aarch64 toolchain と
`-DARPARA_RUST_TARGET=aarch64-unknown-linux-gnu` を指定する。
Rust の対応する target 標準ライブラリと、必要に応じて
`CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER` を用意する。
`CMAKE_CROSSCOMPILING_EMULATOR` を設定した場合は C++・Rust の arpara テストをその
emulator で実行する。未設定の場合、Rust のテストは target のコンパイルのみを行う。

2026-10-10 の追加検証では、aarch64 GNU toolchain（GCC 14.3.0）、Rust 1.99.0、
QEMU 7.2.0 を使い、arpara＋simulated の CLI 最小構成をクロスビルドした。
C++ ドライバと Rust staticlib をリンクした CLI／テスト実行ファイルが
ELF64 little-endian AArch64 であること、CLI に Rust の関数が含まれることを確認した。
QEMU では C++ 模擬 HID・固定姿勢・Rust FFI の3テストが成功し、CLI test も終了コード0。
C++／Rust 両側で構造体のサイズ・アラインメント・field offset を検査している。
検証用ホストに aarch64 Rust 標準ライブラリがなかったため、このクロス検証だけは
インストール済み Rust ソースから標準ライブラリを再ビルドした。通常の Orange Pi 5
ネイティブビルドにはこの作業も `RUSTC_BOOTSTRAP` も不要。
クロス検証ではグラフィックスと USB 探索の外部ライブラリを無効にしているため、
Orange Pi 5 上でのフルランタイム、実機 USB／IMU／映像出力の確認とは区別する。
ログは `build/arpara-validation/arm-build.log`、`arm-tests.log`、`arm-cli-test.log` に保存。

## 公式光学キャリブレーション（2026-10-10）

正本はユーザーがGaming Kitモジュールから取得した `arpara-original.json`。
元ファイルSHA-256は `267b5215f0856749d77ef3ed0acf03bb5ce6f8e434c7a1b93a6bbd4fd27c070c`。
未改変の参照コピーは開発機の `build/arpara-validation/calibration-reference/arpara-original.json` に保存し、
シリアル番号・Lighthouse sensor geometry・機器識別子があるためコミットしない。

確認した全トップレベルキー:
`device`, `device_class`, `device_pid`, `device_serial_number`, `device_vid`, `display_edid`,
`expected_bases`, `expected_controllers`, `firmware_config`, `head`, `imu`, `lighthouse_config`,
`manufacturer`, `model_number`, `render_model`, `revision`, `seconds_from_photons_to_vblank`,
`seconds_from_vsync_to_photons`, `tracking_to_eye_transform`。

取り込み元の位置と値:

| JSON path（i=0:左、i=1:右） | 内容 |
|---|---|
| `device.eye_target_width_in_pixels`, `eye_target_height_in_pixels` | 各2544。眼用画像サイズとして使用。KMSの3840×1920とは別。 |
| `device.first_eye`, `last_eye`, `num_windows` | eEYE_LEFT、eEYE_RIGHT、1 |
| `tracking_to_eye_transform[i].distortion.type` | DISTORT_POLY3 |
| 同 `.coeffs` | [0.172767789, 0.015039393, 0.055597464999999999]（r²、r⁴、r⁶の順） |
| `tracking_to_eye_transform[i].intrinsics` | diag(1.33599997, 1.33599997, 1) |
| 同 `.grow_for_undistort`, `.undistort_r2_cutoff` | 0.600000024、1.39999998 |

両眼の光学値は同一。`distortion_red/blue`、`center_x/y`、`extrinsics`、`eye_to_head`、
`lens_separation`、`physical_aspect_x_over_y` は正本にない。
欠落した色別モデルには記録された共通モデルを使用し、中心はモデルの正規化原点を使う。
色別データが存在するJSONでは独立して読み取るが、この個体の色収差改善を達成したとは扱わない。
aspectは眼用pixel寸法から1.0を導出する。正本のphysical aspect欠落は記録しておき、
square viewportを使う検証済み3840×1920モードでまず評価する。
眼の位置/IPD・canted rotationを推定せず、現在のview pose/IMU originにLighthouseのhead/imu行列を適用しない。

[Valveの公開仕様](https://github.com/ValveSoftware/openvr/wiki/The-JSON-File-(Lighthouse-Devices)) と
このチェックアウトのVive実装を比較した。POLY3は `D(r)=1+k0*r²+k1*r⁴+k2*r⁶`。
`u_compute_distortion_vive()` は逆数型DPOLY3なので、係数をそのまま渡して使用しない。
`u_vive_values` のパラメータ表現とViveの正規化座標／grow・FOV計算を再利用し、
arpara用の薄いアダプタで多項式を評価する。Lighthouse/IMU全体を要求する `vive_config_parse()` は使わない。

per-eye UVはMonadoの既存computeインターフェースと同じ規約を維持し、
`p=(2u-1,(2v-1)/aspect)-center[color]`、`r²=dot(p,p)` とする。
出力は `0.5+(p*D+center[color])*(0.5/(1+grow),0.5*aspect/(1+grow))`。
追加のY反転・左右反転・多項式の数値反転・UVクランプは行わない。
四隅でUVが[0,1]の外に出ることはある。レンダリングテクスチャ外のサンプリングを
内側へ無理に押し込んで正しい補正と見せかけない。
`undistort_r2_cutoff` は正の有限値として保持する。Vive helperもこれを評価式で使用しておらず、
公開資料から正確なmask/clip動作を確認できないため、独自の境界処理は追加しない。
SteamVRの実際のComputeDistortion出力との比較時に境界も検証する。

FOVはintrinsicsを(1+grow)で正規化するViveと同じ方法で計算する。
このJSONでは左右とも水平・垂直100.276239度（各方向±50.138120度）。
focus/centerをFOVに反映することでrender textureの投影とUVのgrowを一致させる。
2544×2544を各眼の `views[].display` に設定するため、アプリの推奨描画サイズが変わる。
既定のscale=140%では約3561×3561/眼となり、以前よりGPU負荷が増える可能性がある。
解像度・画質・FPSを比較し、必要なら既存 `XRT_COMPOSITOR_SCALE_PERCENTAGE` を明示して測定する。
KMS framebuffer／最終合成は引き続き選択モードの3840×1920で、不要な画像コピーは増やさない。

`calibration/import_calibration.py` が正本からdevice/opticsのJSON数値トークンを保持したまま抽出し、
`calibration/optics.json` と元データのSHAを生成した。手作業での係数転記は行っていない。
ビルド時に `calibration/embed_calibration.py` が光学フィールドだけを生成ヘッダへ埋め込む。
Orange Pi 5は開発元のホームディレクトリやLighthouse接続を必要としない。
`-DARPARA_CALIBRATION_JSON=/path/to/local-calibration.json` で個体のデータを差し替えて再ビルドできる。
付属データの別個体への適合性は未確認。別個体にはその個体のデータを指定する。

起動時に一度だけJSONを厳密に解析し、両眼・RGBのcompute callbackを登録して
`u_distortion_mesh_fill_in_compute()` でメッシュを生成する。
`loaded DISTORT_POLY3 optics, source SHA-256=..., eye target=2544x2544; mesh generated` を記録する。
解析失敗／未対応モデルではHMD初期化を失敗させ、歪みなしへ暗黙にfallbackしない。
フレームごとのJSON解析、HID/Rust protocol・IMU fusion・予測・tracking flags、DRM同期の変更はない。

実機手順は既存の全体ビルド／DRM service起動を使う。まずWiVRnを介さないhello_xrで
FOV・立体視・左右中心を確認し、格子／RGBの線を描くOpenXRクライアントで周辺形状と色ずれを確認する。
格子表示クライアントとSteamVR ComputeDistortion参照サンプルは今回の環境にないため、
光学表示テストと数値的なSteamVR一致は未検証。異常な歪みや酔いがあれば装着テストを中止する。
Lighthouse接続後はhead/imu/extrinsicsの原点・軸・相対変換を別途検証する。

今回の自動検証: ホストの全体ビルド成功、CTest 35/35成功。
光学テストは3ケース7579 assertionsで、公式係数の読み込み、左右/RGB独立性、
中央・周辺・四隅の有限値、Valve公開式の独立double計算との比較、不正JSON/モデル/topologyを検証。
模擬HIDテストで初期化・生成メッシュの有限値・3DoF flagsを確認した。
ARM64のドライバ/Rust/CLIクロスビルド成功、QEMUのCTest 24/24成功。
ARMクロス構成は外部グラフィックス/USB依存を無効にしているため、実機フルランタイム検証ではない。
ホストの `monado-cli test` は終了コード0だが、実機arparaのUSB検証ではない。
ログは `build/arpara-validation/optics-build-final.log`, `optics-tests.log`,
`optics-focused.log`, `optics-arm-build.log`, `optics-arm-tests.log`, `optics-cli.log`。

Orange Pi 5でのビルドと最初の表示確認（既存のhello_xrを別端末で起動）:

```sh
cmake -S . -B build -DXRT_HAVE_DRM_KMS=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -R arpara
XRT_COMPOSITOR_FORCE_DRM=1 \
XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 \
XRT_COMPOSITOR_DRM_MODE=3840x1920@71.998 \
XRT_COMPOSITOR_DRM_SYNC_FD=1 \
XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
XRT_COMPOSITOR_LOG=info \
./build/src/xrt/targets/service/monado-service
```

card/connector/GPU番号は既存実機の観測値。環境が変わったらprobeで再確認する。
まず72Hzで補正の形状を確認し、その後モードを `3840x1920@90.001` に切り替えて性能を測る。
推奨解像度が変わるため、以前のFPSを今回の性能測定値として流用しない。
