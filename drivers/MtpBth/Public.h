/*
 * Public.h - MtpBth とユーザーモード（検証ツール・MtpAgent）の間の定義
 *
 * 生フレーム: デバイスインターフェイス GUID_DEVINTERFACE_MTP_RAW を開き ReadFile する。
 *   1 回の ReadFile で 1 レポート（先頭が Report ID 0x31。HIDP ヘッダー 0xA1 は除去済み）を返す。
 *   読み取り要求がないときに届いたレポートは捨て、FramesDropped に数える。
 */
#pragma once

#include <guiddef.h>

/* {7F1C3A52-5B0E-4D7A-9C41-2E836A10D59B} */
DEFINE_GUID(GUID_DEVINTERFACE_MTP_RAW,
    0x7f1c3a52, 0x5b0e, 0x4d7a, 0x9c, 0x41, 0x2e, 0x83, 0x6a, 0x10, 0xd5, 0x9b);

#define MTP_IOCTL_GET_STATUS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef enum _MTP_LINK_STATE {
    MtpLinkIdle = 0,
    MtpLinkConnecting = 1,
    MtpLinkConnected = 2,     /* 両チャネル確立・マルチタッチ有効化済み */
    MtpLinkDisconnected = 3   /* 切断。再接続を待っている */
} MTP_LINK_STATE;

typedef struct _MTP_STATUS_INFO {
    ULONG  LinkState;          /* MTP_LINK_STATE */
    LONG   LastNtStatus;       /* 直近の失敗の NTSTATUS（成功なら 0） */
    ULONG  ConnectAttempts;
    ULONG  EnableHandshake;    /* SET_REPORT への HANDSHAKE 応答（0 = 成功、0xFFFFFFFF = 未受信） */
    ULONG  FramesReceived;     /* 割り込みチャネルで受けた DATA 件数 */
    ULONG  FramesDelivered;    /* ReadFile へ渡した件数 */
    ULONG  FramesDropped;      /* 読み取り要求がなく捨てた件数 */
    ULONGLONG RemoteAddress;   /* BD_ADDR */
} MTP_STATUS_INFO, *PMTP_STATUS_INFO;
