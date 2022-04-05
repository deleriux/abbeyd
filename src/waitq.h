#ifndef _WAITQ_H_
#define _WAITQ_H_

void waitq_init(void);
void waitq_destroy(void);
void waitq_flush(void);
bool waitq_add(class_t cl);
#endif
