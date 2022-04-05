#ifndef _WEBSITE_H_
#define _WEBSITE_H_

void website_init(void);
void website_destroy(void);

class_list_t website_get_timetable(int ndays);
int website_server_time_diff(void);
void website_update_config(void);
int website_wait(class_t cl);
int website_book(class_t cl);
float website_price(class_t cl);
int website_commit(void);
char * website_errbuf(void);
#endif
