/*
 *  Tanto - Object based file system
 *  Copyright (C) 2017  Tanto 
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

#include <unistd.h>
#include <sys/syscall.h> 
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/time.h>

#include <ytrace.h>

/**
 * Internal globals .
 */
static FILE         *ytrace_file;                    /**< ytrace file handle */
static __thread int  ytrace_tid;                             /**< ytrace tid */

/** 
 * Globals.
 */
/* int ytrace_level = YTRACE_DEFAULT;  */                   /**< trace level */
int ytrace_level = YTRACE_LEVEL1;                           /**< trace level */

size_t ytime_get(void)
{
  struct timeval tval;

  gettimeofday(&tval, NULL);

  return (tval.tv_sec * 1000000 + tval.tv_usec);
}

/**
 * @brief Trace initialize function. 
 *        Sets the trace level and sets the trace file handle properly. 
 * 
 * @param None
 * @return None
 */
static void ytrace_init(void)
{
  if (getenv("YTRACE_LEVEL"))
    ytrace_level = atoi(getenv("YTRACE_LEVEL"));

  ytrace_file = stdout;
}

int ytrace_msg_int(int level, const char *func, const char *fmt, ...)
{
  char    *errstr = "";
  char     buf[1024];
  va_list  args;

  va_start(args, fmt);                                      /* do this first */

  if (ytrace_file == NULL)
    ytrace_init();

  if (!ytrace_tid)
    ytrace_tid = syscall(SYS_gettid);

  if (ytrace_file < 0)
    return 0; 

  vsnprintf(buf, sizeof(buf), fmt, args ); 

  va_end( args ); 
  
  if (level == YTRACE_ERROR)
    errstr = "ERROR:";

  fprintf(ytrace_file, "%llu-%d : %-16s : %s %s", 
          (long long unsigned int)ytime_get(), ytrace_tid, 
	  func, errstr, buf);

  fflush(ytrace_file);
}
