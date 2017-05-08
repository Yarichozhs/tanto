/*
 *  Yari - In memory Key Value Store 
 *  Copyright (C) 2017  Yari 
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _YTRACE_H

#include <errno.h>
#include <stdio.h>

#define _YTRACE_H

/**
 * Internal ytrace levels.
 */
#define YTRACE_ERROR   -1 
#define YTRACE_DEFAULT  0
#define YTRACE_LEVEL1   1
#define YTRACE_LEVEL2   2

/**
 * Trace level variable. 
 */
extern int ytrace_level;

size_t ytime_get(void);

/**
 * @brief Internal trace routine. Do not use this directly.
 *  Use ytrace_msg instead. 
 * 
 * @param level - Trace level
 * @param func  - Function name
 * @param fmt   - Format specifier
 * @return number of characters traced. 
 */
int ytrace_msg_int(int level, const char *func, const char *fmt, ...);

/**
 * @brief Trace function. 
 * 
 * @param level - Trace level
 * @param fmt   - Format specifier
 * @return None
 */
#define ytrace_msg(level, ...) \
        if (level <= ytrace_level) \
	  ytrace_msg_int(level, __func__,  __VA_ARGS__)

#endif /* ytrace.h */
