#ifndef _DATABASE_H_
#define _DATABASE_H_

#include "class.h"

void database_init(void);
void database_destroy(void);

int database_start(void);
int database_rollback(void);
int database_commit(void);

class_t database_get(int classid);
int database_add(class_t cl);

#endif
