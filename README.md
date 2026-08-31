# ZMK Config for Kugel-1 (BLE Micro Pro Dedicated)

Kugel-1 自作キーボード用の **BLE Micro Pro (BMP)** 専用 ZMK Firmware リポジトリです。  
Bluetooth Low Energy (BLE) および USB で動作し、**ZMK Studio** によるグラフィカルなキーマップ変更、**Prospector Scanner** による外付けステータス表示、単三乾電池のバッテリー残量監視、QMK 互換の PCB LED インジケーターにも完全対応しています。

---

## 🚀 ハードウェア構成と主な特徴

- **MCU / ボード**: BLE Micro Pro (nRF52840 / BL654)
- **キーマトリックス**:
  - 3個の MCP23S17（SPI接続 IO エキスパンダ、HAEN アドレス方式）による 43キーダイレクトスキャン
  - Kugel-1 専用 ZMK Kscan ドライバ (`zmk,kscan-kugel`) による高速・低遅延キースキャン
- **ポインティングデバイス (トラックボール)**:
  - **Bit Trade One ADTB7M (PixArt PAW3204)** 1U トラックボール（標準・動作確認済み）
  - **直感的なマウス操作レイヤー**: デフォルトレイヤーの `.` キー長押しで素早く `MOUSE` レイヤーに切り替え、右手ホームポジション周辺（`U`=左クリック、`O`=右クリック、`P`=中クリック）でシームレスに操作可能
  - **モーメンタリ・スクロール (`&tb TB_SCRL_MOM`)**: `MOUSE` レイヤー中に Lower キーを押している間だけスクロールモードに移行し、離すと即座にポインター移動に戻る快適な操作感
  - **オートマウスレイヤー機能**: ボールを回すと自動的に `MOUSE` レイヤーへ遷移し、停止800msまたはタイピング再開で即時復帰
  - **スナイパーモード**: `SNIPE` レイヤー突入時に超低速（通常の3倍遅い精密除数）へ自動減速
  - **動的ポインター速度調整**: キー操作で 4 段階（Level 1〜4）の速度変更が可能（NVS 設定永続化対応）
  - **ポインター加速度（Mouse Acceleration）**: 微小移動時の精密操作とフリック時の高速移動を両立
  - **スクロール軸ロック**: 縦スクロール時の横ブレ暴発を防止
- **単三乾電池（1本）バッテリー残量管理**:
  - `zmk-feature-non-lipo-battery-management` 外部モジュールを `west.yml` 経由で統合
  - QMK 純正設定に合わせた電圧測定レンジ（800mV = 0%、1300mV = 100%）により、単三アルカリ乾電池および NiMH（エネループ）の正確な残量を Windows / OS の Bluetooth Battery Service (BAS) へリアルタイム報告
  - USB 接続時は保護回路による電圧降下を検出し、100%（外部電源駆動）としてスマートにフォールバック表示
- **QMK 完全互換 PCB ステータス LED (D100 / P0.08)**:
  - ハードウェア UART TX の競合を排他制御し、基板上の青色 LED を GPIO インジケーターとして完全解放
  - 未接続時: 5 秒周期の 2 連点滅
  - BLE 接続時: 300ms 点灯
  - 起動時: バッテリー残量に応じた点滅回数通知（QMK 同等シーケンス）
- **Prospector Scanner ステータス表示連携**:
  - `prospector-zmk-module` を `west.yml` 経由で導入
  - BLE Advertisement により、Bluetooth の接続スロット（最大5台）を消費することなく、外付けディスプレイ端末（Prospector Scanner）へキーボード名、レイヤー、プロファイル、バッテリー残量、WPM などをリアルタイム配信
- **ZMK Studio 完全対応**:
  - 実機のエルゴノミクス曲線を 1:1 で再現した物理キーレイアウト定義 (`key_physical_attrs`)
  - USB / BLE 双方でのリアルタイムかつグラフィカルなキーマップ変更に対応

---

## 📁 ディレクトリ構成

```
.
├── boards/arm/ble_micro_pro/       # BLE Micro Pro ボード定義一式 (HWMv2)
│   ├── board.cmake / board.yml
│   ├── ble_micro_pro_nrf52840.dts  # UART0解放・AIN3単三電池・GPIOアサイン
│   ├── ble_micro_pro_nrf52840_defconfig
│   ├── Kconfig.ble_micro_pro / Kconfig.defconfig
│   └── arduino_pro_micro_pins.dtsi / ble_micro_pro-pinctrl.dtsi
├── drivers/
│   ├── kscan/
│   │   └── kscan_kugel.c           # Kugel-1 MCP23S17 HAEN スキャンドライバ
│   ├── indicator/
│   │   └── kugel_indicator.c       # QMK互換 PCB LED (D100) 点滅制御ドライバ
│   └── sensor/
│       └── paw3204/                # PAW3204 ドライバ・拡張制御・Behavior一式
│           ├── paw3204.c / paw3204.h
│           ├── paw3204_control.c / paw3204_control.h
│           └── behavior_trackball.c
├── dts/bindings/
│   ├── kscan/
│   │   └── zmk,kscan-kugel.yaml    # Kscan DTS バインディング
│   ├── sensor/
│   │   └── bto,paw3204.yaml        # PAW3204 DTS バインディング
│   └── behaviors/
│       └── zmk,behavior-trackball.yaml # トラックボール制御 Behavior バインディング
├── include/dt-bindings/zmk/
│   └── trackball.h                 # トラックボール操作用コマンド定義ヘッダー
├── config/
│   ├── west.yml                    # 外部モジュール管理 (non-lipo-battery, prospector)
│   └── boards/shields/kugel/
│       ├── Kconfig.shield / Kconfig.defconfig
│       ├── kugel.zmk.yml           # ZMK Studio レイアウト定義メタデータ
│       ├── kugel.dtsi              # 物理レイアウト・ハードウェア接続定義
│       ├── kugel.overlay           # ピンアサイン・SPIマッピング
│       ├── kugel.conf              # Kconfig 設定 (BLE/Pointing/Studio/Battery/Prospector)
│       └── kugel.keymap            # キーマップ定義（正本）
├── .github/workflows/build.yml     # GitHub Actions 自動ビルドワークフロー
├── build-local.sh                  # ローカル WSL + Docker ビルドスクリプト（ポータブル）
├── build-local.bat                 # Windows 用ワンクリックビルドバッチ（汎用化済み）
├── build.yaml                      # ビルドマトリックス (ble_micro_pro)
├── CMakeLists.txt / Kconfig        # モジュールビルド設定
└── zephyr/module.yml               # Zephyr モジュールエントリー
```

---

## ⌨️ レイヤー構成

- **Layer 0: Default**
  - 基本英字入力（QWERTY）
  - 右手親指キーでのマウスクリック（`LCLK`, `RCLK`）
  - `.` キー長押しで **Layer 4 (Mouse)** へクイック移行
- **Layer 1: Lower**
  - 数字・記号入力
  - `N3` キー長押しで **Layer 5 (Snipe)** 超精密エイムモードへ移行
- **Layer 2: Raise**
  - ファンクションキー（F1〜F12）・カーソル移動
  - キー長押しでトラックボールスクロール
- **Layer 3: Adjust**
  - BLE プロファイル切り替え（Profile 0〜4）
  - USB / BLE 出力トグル
  - トラックボール速度調整（Level 1〜16: 2.50x〜0.19x）、スクロール感度調整（Level 1〜6: 超滑らか〜高速）、2次関数スムーズ加速ON/OFFトグル、オートマウス有効/無効トグル（設定は NVS に自動永続化）
- **Layer 4: Mouse**
  - トラックボール操作または `.` キー長押しで突入
  - ホームポジション周辺でマウス操作: `U`（左クリック）、`O`（右クリック）、`P`（中クリック）
  - `Lower` キーを押している間: **モーメンタリ・スクロール (`&tb TB_SCRL_MOM`)**
  - 進む / 戻る、スナイパー、スクロールトグルキーを配置
- **Layer 5: Snipe**
  - 超低速・精密エイムモード（通常の 1/3 の速度）

---

## 🛠️ ローカルでのビルド方法 (WSL + Docker)

スクリプトはユーザー環境に依存しない汎用的な設計になっています。

### 1. Windows の場合
リポジトリ直下の **`build-local.bat`** をダブルクリックして実行します。
（WSL 内の Docker コンテナが自動起動し、ファームウェアをコンパイルします）

### 2. WSL / Linux の場合
ターミナルから以下のコマンドを実行します：
```bash
./build-local.sh
```

ビルドが完了すると、`build/artifacts/` フォルダ内にファームウェアが生成されます：
- **`build/artifacts/kugel_ble_micro_pro.uf2`**

---

## 📥 書き込み手順

1. BLE Micro Pro を USB ケーブルで PC に接続します。
2. リセットボタンを素早く **2回押し**（ダブルタップ）して、ブートローダーモード（マスストレージドライブ）に入ります。
3. PC に認識されたドライブ（**`BLEMICROPRO`**）に、生成された `build/artifacts/kugel_ble_micro_pro.uf2` をドラッグ＆ドロップします。
4. 書き込みが完了すると、自動的に再起動して Kugel-1 として動作を開始します。

---

## ⚠️ 電源管理とスリープに関する技術メモ (Known Issue / Memo)

### ZMK Studio と `CONFIG_ZMK_SLEEP` の共存不具合について
現在、Zephyr 4.1 ベースの ZMK main において、**ZMK Studio（`CONFIG_ZMK_STUDIO=y`）とディープスリープ（`CONFIG_ZMK_SLEEP=y`）を同時に有効にすると、スリープ移行時に電源管理（`pm.c` / `pm_device_slots`）のメモリ違反によりマイコンがハングアップしてキー復帰できなくなる不具合**（上流 GitHub Issues #3195, #3207）が存在します。

そのため、現在の本リポジトリでは安定運用のために **`CONFIG_ZMK_SLEEP=n`（ディープスリープオフ）** に設定し、**System ON 省電力アイドル待機（WFI）** で運用しています。
- **電池持ちへの影響**: アイドル待機（30秒放置で省電力モード・50msスキャン待機へ移行）でも、単三乾電池（エネループ等）1本で **日常使用で約 3 ヶ月、完全放置で約 5〜6 ヶ月** 動作します（旧 QMK ファームウェアと同等の運用形態です）。
- **メリット**: スリープによる Bluetooth 切断や復帰遅延がなく、キーを押した瞬間に遅延ゼロ（1ミリ秒）で即座に入力されます。

### 将来のアップデート方針
ZMK / Zephyr 上流で上記バグ（Issue #3195）が修正され、ZMK Studio とディープスリープの共存が安定動作するようになった段階で、`config/boards/shields/kugel/kugel.conf` の **`CONFIG_ZMK_SLEEP=y`** を再有効化する予定です。

