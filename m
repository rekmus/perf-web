#!/bin/sh

. ./dev.env

g++ silgy_app.cpp silgy_eng.c silgy_lib.c $WEB_INCLUDE_PATH $WEB_CFLAGS -o $SILGYDIR/bin/silgy_app
