# ZMK Config for Kugel-1 (BLE Micro Pro Dedicated)

Kugel-1 自作キーボード用の **BLE Micro Pro (BMP)** 専用 ZMK Firmware リポジトリです。
Bluetooth Low Energy (BLE) および USB で動作し、**ZMK Studio** によるグラフィカルなキーマップ変更にも完全対応しています。

---

## ハードウェア構成と特徴

- **MCU / ボード**: BLE Micro Pro (nRF52840 / BL654)
- **キーマトリックス**:
  - 3個の MCP23S17（SPI接続 IO エキスパンダ、HAEN アドレス方式）による 43キーダイレクトスキャン
  - Kugel-1 専用 ZMK Kscan ドライバ (`zmk,kscan-kugel`) による高速・確実なキースキャン
- **ポインティングデバイス (トラックボール)**:
  - **Bit Trade One ADTB7M (PixArt PAW3204)** 1U トラックボール（標準・動作確認済み）
  - ※ ADNS-7530 トラックボールドライバもリポジトリ内に保持していますが、実機での動作確認は行われていません（未検証）
  - **オートマウスレイヤー機能**: ボールを回すと自動的に `MOUSE` レイヤーへ遷移し、停止800msまたはタイピング再開で即時復帰
  - **スナイパーモード**: `SNIPE` レイヤー突入時に超低速（通常の3倍遅い精密除数）へ自動減速
  - **動的ポインター速度調整**: キー操作で 4 段階（Level 1〜4）の速度変更が可能
  - **NVS 設定永続化**: 速度設定およびオートマウス有効/無効状態を Flash メモリ（NVS）に自動保存
  - **ポインター加速度（Mouse Acceleration）**: 微小移動時の精密操作とフリック時の高速移動を両立
  - **スクロール軸ロック**: 縦スクロール時の横ブレ暴発を防止
  - **トグル式ドラッグスクロール**: タップでスクロールモードに入り、クリックで自動解除
- **ZMK Studio 対応**:
  - 実機のエルゴノミクス曲線を 1:1 で再現した物理キーレイアウト定義 (`key_physical_attrs`)
  - USB / BLE 双方での ZMK Studio キーマップ変更に対応

---

## ディレクトリ構成

```
.
├── boards/arm/ble_micro_pro/       # BLE Micro Pro ボード定義一式 (HWMv2)
│   ├── board.cmake / board.yml
│   ├── ble_micro_pro_nrf52840.dts
│   ├── ble_micro_pro_nrf52840_defconfig
│   ├── Kconfig.ble_micro_pro / Kconfig.defconfig
│   └── arduino_pro_micro_pins.dtsi / ble_micro_pro-pinctrl.dtsi
├── drivers/
│   ├── kscan/
│   │   └── kscan_kugel.c           # Kugel-1 MCP23S17 HAEN スキャンドライバ
│   └── sensor/
│       ├── paw3204/                # PAW3204 ドライバ・拡張制御・Behavior一式
│       │   ├── paw3204.c / paw3204.h
│       │   ├── paw3204_control.c / paw3204_control.h
│       │   └── behavior_trackball.c
│       └── adns7530/               # ADNS-7530 トラックボールドライバ (保持)
├── dts/bindings/
│   ├── kscan/
│   │   └── zmk,kscan-kugel.yaml    # Kscan DTS バインディング
│   ├── sensor/
│   │   ├── bto,paw3204.yaml        # PAW3204 DTS バインディング
│   │   └── adns,adns7530.yaml      # ADNS-7530 DTS バインディング
│   └── behaviors/
│       └── zmk,behavior-trackball.yaml # トラックボール制御 Behavior バインディング
├── include/dt-bindings/zmk/
│   └── trackball.h                 # トラックボール操作用コマンド定義ヘッダー
├── config/
│   ├── west.yml
│   └── boards/shields/kugel/
│       ├── Kconfig.shield / Kconfig.defconfig
│       ├── kugel.zmk.yml           # ZMK Studio レイアウト定義メタデータ
│       ├── kugel.dtsi              # 物理レイアウト・ハードウェア接続定義
│       ├── kugel.overlay           # ピンアサイン・SPIマッピング
│       ├── kugel.conf              # Kconfig 設定 (BLE/Pointing/Studio)
│       └── kugel.keymap            # キーマップ定義
├── .github/workflows/build.yml     # GitHub Actions 自動ビルドワークフロー
├── build-local.sh                  # ローカル WSL + Docker ビルドスクリプト
├── build-local.bat                 # Windows 用ビルドバッチ
├── build.yaml                      # ビルドマトリックス (ble_micro_pro)
├── CMakeLists.txt / Kconfig        # モジュールビルド設定
└── zephyr/module.yml               # Zephyr モジュールエントリー
```

---

## レイヤー構成

- **Layer 0: Default** - 基本英字入力（QWERTY）、親指マウスクリック（`LCLK`, `RCLK`）
- **Layer 1: Lower** - 数字・記号入力、`N3` 長押しで `SNIPE` レイヤー
- **Layer 2: Raise** - ファンクションキー・カーソル移動、長押しでトラックボールスクロール
- **Layer 3: Adjust** - BLEプロファイル切り替え、USB/BLE出力トグル、トラックボール調整（速度変更、オートマウストグル、スクロールトグル）
- **Layer 4: Mouse** - トラックボールを回すと自動突入。進む/戻る、中クリック、スナイパー、スクロールトグル
- **Layer 5: Snipe** - 超低速・精密エイムモード

---

## ローカルでのビルド方法 (WSL + Docker)

### 1. Windows の場合
`build-local.bat` をダブルクリックします。

### 2. WSL / Linux の場合
```bash
./build-local.sh
```

ビルドが完了すると、`build/artifacts/` フォルダ内にファームウェアが生成されます：
- `build/artifacts/kugel_ble_micro_pro.uf2`

---

## 書き込み手順

1. BLE Micro Pro を USB ケーブルで PC に接続します。
2. リセットボタンを素早く **2回押し**（ダブルタップ）して、ブートローダーモード（マスストレージドライブ）に入ります。
3. PC に認識されたドライブ（`BLEMICROPRO`）に、生成された `build/artifacts/kugel_ble_micro_pro.uf2` をドラッグ＆ドロップします。
4. 書き込みが完了すると、自動的に再起動して Kugel-1 として動作を開始します。
