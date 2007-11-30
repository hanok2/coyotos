#ifndef __KERNINC_COMMANDLINE_H__
#define __KERNINC_COMMANDLINE_H__
/*
 * Copyright (C) 2007, The EROS Group, LLC.
 *
 * This file is part of the Coyotos Operating System runtime library.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330 Boston, MA 02111-1307, USA.
 */

/**
 * @file
 *
 * @brief Command line processing interface.
 */

#include <stdbool.h>
#include <kerninc/ccs.h>

#define COMMAND_LINE_LIMIT 1024

extern char CommandLine[];

void cmdline_process_options();

const char *cmdline_argv0();
bool cmdline_has_option(const char *optName);
const char *cmdline_option_arg(const char *optName);
bool cmdline_option_isstring(const char *optname, const char *value);
unsigned long cmdline_option_uvalue(const char *optname);


#endif /* __KERNINC_COMMANDLINE_H__ */
