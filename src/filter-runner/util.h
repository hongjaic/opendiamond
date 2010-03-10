/*
 *  The OpenDiamond Platform for Interactive Search
 *  Version 5
 *
 *  Copyright (c) 2002-2007 Intel Corporation
 *  Copyright (c) 2006 Larry Huston <larry@thehustons.net>
 *  Copyright (c) 2006-2010 Carnegie Mellon University
 *  All rights reserved.
 *
 *  This software is distributed under the terms of the Eclipse Public
 *  License, Version 1.0 which can be found in the file named LICENSE.
 *  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
 *  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
 */

#ifndef OPENDIAMOND_SRC_FILTER_RUNNER_UTIL_H_
#define OPENDIAMOND_SRC_FILTER_RUNNER_UTIL_H_

#include <glib.h>
#include <stdlib.h>

#include "util.h"


int get_size(FILE *in);

char *get_string(FILE *in);

char **get_strings(FILE *in);

void *get_blob(FILE *in, int *bloblen_OUT);

void send_tag(FILE *out, const char *tag);

void send_int(FILE *out, int i);

void send_string(FILE *out, const char *str);

#endif
