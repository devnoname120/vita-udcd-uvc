#ifndef AUDIO_H
#define AUDIO_H

#include <psp2kern/udcd.h>

int uac_audio_init(SceUdcdEndpoint *endpoint);
int uac_audio_start(void);
void uac_audio_request_stop(void);
int uac_audio_stop_sync(void);
void uac_audio_begin_shutdown(void);
void uac_audio_on_attach(void);
int uac_audio_term(void);

#endif
