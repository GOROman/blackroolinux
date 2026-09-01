# M5Stack CoreS3 PS1 SIO1 bridge

M5Stack CoreS3 を PS1 の SIO1 シリアルアダプターとして使うプロジェクトです。

## 機能

- BLE HID Host として BLE キーボードを接続
- BLE キーボードの HID レポートを PS1 SIO1 へ送信
- USB-C の USB Serial/JTAG CDC と PS1 SIO1 の双方向ブリッジ
- PS1 側は CoreS3 の G44/G43 を使用

## 配線

| CoreS3 | PS1 SIO1 | 用途 |
|---|---|---|
| G43 | RX | CoreS3 TX → PS1 RX |
| G44 | TX | CoreS3 RX ← PS1 TX |
| GND | GND | 共通グランド |

G43/G44 は ESP32-S3 の GPIO 番号です。PS1 側の端子形状は専用8ピンの
Serial I/O コネクタなので、専用ブレークアウトケーブルを使ってください。

電圧は 3.3V TTL を前提にします。RS-232 ±12V や 5V TTL を直接接続しないでください。

## ビルドと書き込み

```sh
pio run
pio run -t upload
pio device monitor -b 115200
```

または ESP-IDF で:

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

CoreS3 の USB-C を Mac に接続すると、USB CDC 経由で PS1 SIO1 のログを受信でき、
Mac から送ったバイト列は PS1 に送信されます。

## 使い方

1. CoreS3 を USB-C で接続する。
2. PS1 SIO1 と G43/G44/GND を接続する。
3. CoreS3 を起動し、BLE キーボードをペアリングする。
4. PS1 側で Blackroo Linux の `blackroo>` またはシェルを起動する。
5. Mac 側は `pio device monitor -b 115200` でログと入力を確認する。

## 注意

PS1 の SIO1 は一般的な UART 端子ではなく、Blackroo 側のプロトコル・配線条件に
合わせる必要があります。BLE HID のレポート形式もキーボード機種差があるため、
最初は USB シリアル経由で `blackroo>` の応答を確認してから BLE 入力を試します。
