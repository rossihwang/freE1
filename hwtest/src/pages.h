#ifndef E1_PAGES_H
#define E1_PAGES_H

#include "app.h"

void pages_build(void);
void pages_show(int idx);
void pages_next(int dir);
int  pages_current(void);
void pages_update(void);     /* dynamic content, called every ~30 ms */
void pages_finish(void);     /* write report, restore factory app */

#endif
