#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// 基本ランタイム設定
#define HAVE_MALLOC
#define HAVE_EMBEDDED_TIME_MS

// BLE 機能を有効化
#ifndef ENABLE_BLE
#define ENABLE_BLE
#endif

#define ENABLE_LE_PERIPHERAL
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// Pico W (CYW43ドライバ) 必須設定
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

// ペアリング・デバイスDB設定
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4
#define NVM_NUM_DEVICE_DB_ENTRIES   4

// メモリ・バッファ設定
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_SERVICE_RECORD_ITEMS 1

// 【BL-43】cyw43 共有バス オーバーラン防止 (上流 pico-examples 準拠)
// コントローラ ACL バッファを抑制し、HCI Controller-to-Host フロー制御を有効化する
// 最大 6-8 回の接続/切断サイクルでスタックが沈黙する既知事例 (pico-sdk #2181 / btstack #654) の対策
// SCO は未使用だがフロー制御の必須ペアとして上流例の値をそのまま設定する
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// セキュリティ・暗号化
#define ENABLE_LE_SECURE_CONNECTIONS

#endif