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
#ifndef _REDISLIB_H

#define REDIS_SERVER_DEFAULT_IP    "127.0.0.1"
#define REDIS_SERVER_DEFAULT_PORT  6379

#define _REDISLIB_H

#define REDIS_KEY_LEN (512)

struct redis_ctx_t
{
  int sfd;
};
typedef struct redis_ctx_t redis_ctx_t;

int redis_connect(redis_ctx_t *ctx, char *ip, int port);
int redis_get(redis_ctx_t *ctx, char *key, int klen, void *val, int vlen);
int redis_set(redis_ctx_t *ctx, char *key, int klen, void *val, int vlen);
int redis_keys(redis_ctx_t *ctx, char *pat, int plen, char *keys[REDIS_KEY_LEN], int nkeys);
int redis_del(redis_ctx_t *ctx, char *key, int klen);
int redis_close(redis_ctx_t *ctx);

#endif /* redislib.h */
