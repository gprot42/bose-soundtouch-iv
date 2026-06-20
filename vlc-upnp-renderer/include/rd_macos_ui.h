/*
 * rd_macos_ui.h — schedule work on the macOS main run loop
 */
#ifndef RD_MACOS_UI_H
#define RD_MACOS_UI_H

typedef void (*rd_ui_fn)(void *userdata);

void rd_macos_run_on_ui(rd_ui_fn fn, void *userdata);
void rd_macos_run_on_ui_sync(rd_ui_fn fn, void *userdata);

#endif /* RD_MACOS_UI_H */