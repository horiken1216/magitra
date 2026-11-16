/*
 * mtp_input.h - mtp_gesture のイベントを SendInput で Windows に渡す
 *
 * 注意: 管理者権限で動いているウィンドウには、権限なしのプロセスからは入力を送れない（Windows の仕様）。
 */
#ifndef MTP_INPUT_H
#define MTP_INPUT_H

#include <windows.h>

#include "mtp_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 件送る。成功なら TRUE。失敗時は GetLastError() で理由が分かる */
BOOL MtpInputSend(const MTP_EVENT *ev);

#ifdef __cplusplus
}
#endif

#endif /* MTP_INPUT_H */
