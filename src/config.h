#ifndef _CONFIG_H_
#define _CONFIG_H_
#include "class.h"

void config_parse(const char *conf);
void config_unload(void);
void config_reload(void);

char * config_get_login(void);
char * config_get_password(void);
char * config_get_db_path(void);
char * config_get_location(void);
int config_get_max_days(void);
char * config_get_cookies(void);
int config_get_waitlist_timeout(void);

int config_get_num_classes(void);
class_list_t config_get_classes(void);
struct tm * config_get_waketime(void);
#endif
