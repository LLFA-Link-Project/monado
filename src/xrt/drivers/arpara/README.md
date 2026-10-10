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
  position tracking は宣言しない。位置はゼロ、position valid/tracked フラグは付けない。
- IMU が届くまでは identity orientation を返し、tracked を付けない。
  `ARPARA_IMU=false` は送信も読取スレッドも使わない固定姿勢の登録確認モード。
- 画面設定は登録のためだけの仮値：全体2560×1440、60Hz、水平FOV 90°、
  画面寸法130×73mm、レンズ間隔65mm、レンズ高さ36.5mm、歪み補正なし。
  **実機の解像度・リフレッシュレート・IPD・光学特性を確定した値ではない。**
  IPD は view pose 呼出し元の default eye relation を使う。
  今後の光学実装は `~/ArparaHIDProtocolReverseEnginiering/arpara-original.json`
  を参照し、実機構成と座標規約を確認してから適用する。
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
