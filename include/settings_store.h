#ifndef C77_SETTINGS_STORE_H
#define C77_SETTINGS_STORE_H
#include "keyboard.h"
enum { SETTINGS_ERROR_NONE=0,SETTINGS_ERROR_ARGUMENT,SETTINGS_ERROR_UNLOCK,SETTINGS_ERROR_ERASE,SETTINGS_ERROR_PROGRAM,SETTINGS_ERROR_VERIFY,SETTINGS_ERROR_CACHE,SETTINGS_ERROR_LAYOUT };
int settings_store_load(c77_config *profile,uint32_t *generation,bool *output_enabled);
int settings_store_save(const c77_config *profile,uint32_t *generation,bool output_enabled);
uint32_t settings_store_last_error(void);
#endif
