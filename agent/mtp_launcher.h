/*
 * mtp_launcher.h - ランチャー（mtplauncher.exe、launcher/）の起動と、表示の知らせ
 *
 * ランチャーは常駐アプリと同じフォルダーに置き、常駐アプリの起動時に隠れたまま起動しておく（WPF の起動待ちを避ける）。
 * 知らせは登録メッセージ MTP_LAUNCHER（wParam = MTP_LAUNCHER_CMD）。番号は launcher/MtpLauncher.cs と同じにすること。
 */
#ifndef MTP_LAUNCHER_H
#define MTP_LAUNCHER_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_LAUNCHER_EXE          L"mtplauncher.exe"
#define MTP_LAUNCHER_WINDOW_TITLE L"MTP Launcher"
#define MTP_MSG_LAUNCHER          L"MTP_LAUNCHER"

typedef enum MTP_LAUNCHER_CMD {
    MTP_LAUNCHER_SHOW = 1,
    MTP_LAUNCHER_HIDE = 2,
    MTP_LAUNCHER_QUIT = 3
} MTP_LAUNCHER_CMD;

/* 動いていなければ、隠れたまま起動しておく */
void MtpLauncherStart(void);

/* 表示する。動いていなければ、表示する指示を付けて起動する。成功なら TRUE */
BOOL MtpLauncherShow(void);

/* 常駐アプリの終了時に、ランチャーも終わらせる */
void MtpLauncherQuit(void);

#ifdef __cplusplus
}
#endif

#endif /* MTP_LAUNCHER_H */
