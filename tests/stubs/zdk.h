#ifndef TEST_ZDK_H
#define TEST_ZDK_H
#include "windows.h"
#define FAILED(result) ((result) < 0)
int ZDKCloud_IsConnected(BOOL *);
int ZDKCloud_Connect();
#endif
