#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>   
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <redislib.h>

#define REDIS_OK_STR "+OK"
#define REDIS_OK_LEN  3

#define REDIS_NULL      "$-1\r\n"
#define REDIS_NULL_LEN  (sizeof(REDIS_NULL) - 1)

#define TRUE 1
#define FALSE 0

int redis_connect(redis_ctx_t *ctx, char *ip, int port)
{
  char   buffer[256];
  struct sockaddr_in serv_addr;

  if (ip == NULL)
    ip = REDIS_SERVER_DEFAULT_IP;

  if (port == 0)
    port = REDIS_SERVER_DEFAULT_PORT;

  ctx->sfd = socket(AF_INET, SOCK_STREAM, 0);

  if (ctx->sfd < 0)
  {
    printf("socket creation failed : %d\n", errno); 
    return -1;
  }

  bzero((char *) &serv_addr, sizeof(serv_addr));

  serv_addr.sin_family      = AF_INET;
  serv_addr.sin_addr.s_addr = inet_addr(ip);
  serv_addr.sin_port        = htons(port);

  if (connect(ctx->sfd, (struct sockaddr *) &serv_addr,
              sizeof(serv_addr)) < 0)
  {
    printf("connect failed : %d\n", errno); 
    close(ctx->sfd);
    ctx->sfd = -1;

    return -1;
  }

 // printf("connected to ip = %s port = %d, fd = %d\n", ip, port, ctx->sfd);

  return 0;
}

#define REDIS_MAX_LEN (8192 )

int redis_get(redis_ctx_t *ctx, char *key, int klen, void *val, int vlen)
{
  int          rc;
  int          rem;
  int          len;
  int          rlen;
  int          parsed;
  char        *sp;
  char         buf[64];
  int          to_read;
  int          copied;
  struct iovec iovec[5];

  //printf("redis_get : key [%.*s] %d\n", klen, key, klen); 

  iovec[0].iov_base = "*2\r\n";
  iovec[0].iov_len  = strlen(iovec[0].iov_base);

  iovec[1].iov_base = "$3\r\nGET\r\n";  /* GET - 3 char */
  iovec[1].iov_len  = strlen(iovec[1].iov_base);

  sprintf(buf, "$%d\r\n", klen);
  iovec[2].iov_base = buf;
  iovec[2].iov_len  = strlen(buf);;

  iovec[3].iov_base = key;
  iovec[3].iov_len  = klen;

  iovec[4].iov_base = "\r\n";
  iovec[4].iov_len  = 2;

  if ((rc = writev(ctx->sfd, iovec, sizeof(iovec)/sizeof(iovec[0]))) < 0)
    return -1;

  if ((rlen = read(ctx->sfd, buf, sizeof(buf))) < 0)
    return -1;

  //printf("read = [%d] [%s]\n", rlen, buf);

  if ((rc = sscanf(buf, "$%d\r\n%n", &len, &parsed)) != 1)
    return -1;

  if (len == -1)
    return -1;

  to_read  = parsed + len + 2;         /* total amount to read from socket */
  to_read -= rlen;                                     /* read rlen so far */

  if (1)                                       /* Copy the read one so far */
  {
    rem = rlen - parsed;

    if (rem > len)                            /* value is smaller than buf */
      rem = len;

    if (rem > vlen)
      rem = vlen;

    memcpy(val, &buf[parsed], rem);

    copied = rem;

    vlen -= rem;
    val   = (void *)&((char *)val)[rem];
  }

  if (vlen && to_read && (len - copied))
  {
    rem = len - copied;  /* remaining data to read */

    if (rem > vlen)
      rem = vlen;

    if (read(ctx->sfd, val, rem) != rem)
      return -1;

    copied += rem;

    to_read -= rem;
    vlen -= rem;
  }

  while (to_read)
  {
    rc = read(ctx->sfd, buf, sizeof(buf));
      
    if (rc == -1)
      break;

    to_read -= rc;
  }

  return copied;
}

struct redis_buf_t
{
  char   buf[REDIS_KEY_LEN * 2];
  int    cur;
  int    rem;
};
typedef struct redis_buf_t redis_buf_t;

int redis_buf_consume(redis_buf_t *rbuf, char *buf, int n)
{
  if (rbuf->rem < n)
    n = rbuf->rem;

  if (n)
  {
    memcpy(buf, &rbuf->buf[rbuf->cur], n);
    rbuf->cur += n;
    rbuf->rem -= n;
  }

  return n;
}

int redis_buf_peek(redis_buf_t *rbuf, char *buf, int n)
{
  if (rbuf->rem < n)
    n = rbuf->rem;

  if (n)
  {
    memcpy(buf, &rbuf->buf[rbuf->cur], n);
  }

  return n;
}

int redis_buf_fill(redis_ctx_t *ctx, redis_buf_t *rbuf)
{
  int ret;

  if (rbuf->rem)
    return rbuf->rem;

  ret = read(ctx->sfd, rbuf->buf, sizeof(rbuf->buf));

  if (ret < 0)
    return -1;

  rbuf->cur = 0;
  rbuf->rem = ret;

  return ret;
}

int redis_read_int(redis_ctx_t *ctx, redis_buf_t *rbuf, int *rval)
{
  int val = 0;
  char c;

  while (TRUE)
  {
    if (redis_buf_fill(ctx, rbuf) < 0) 
      break;

    if (redis_buf_peek(rbuf, &c, 1) != 1)
      break;

    if (c >= '0' && c <= '9')
    {
      val = val * 10 + (c - '0');
      redis_buf_consume(rbuf, &c, 1);  /* discard */
    }
    else 
      break;
  }

  *rval = val;

  return val;
}

int redis_read_sep(redis_ctx_t *ctx, redis_buf_t *rbuf)
{
  int ret = -1;
  char c[2];

  if (redis_buf_fill(ctx, rbuf) < 0) 
    return -1;

  if (redis_buf_peek(rbuf, c, 2) != 2)
    return -1;

  ret = (c[0] == '\r' && c[1] == '\n') ? 0 : -1;

  redis_buf_consume(rbuf, c, 2); /* discard */
}

int redis_read_data(redis_ctx_t *ctx, void *ptr[], size_t size[], int n)
{
  char  *bp = NULL;
  char  *ep = NULL;
  int    rem;
  int    done = 0;
  int    nrec = 1;
  int    crec = 0;
  void   *tptr;
  int     tsize;
  int     tlen;
  int      len;
  redis_buf_t rbuf;
  char buf[512];
  char  c;

  rbuf.rem = 0;

  while (nrec)
  {
    redis_buf_fill(ctx, &rbuf);

    if (redis_buf_consume(&rbuf, &c, 1) == -1)
      return -1;

    switch (c)
    {
      case '*':
      {
        if (redis_read_int(ctx, &rbuf, &nrec) < 0)
	  return -1;
        
	if (redis_read_sep(ctx, &rbuf) < 0)
	  return -1;

        break;
      }
      case '$':
      {
        if (redis_read_int(ctx, &rbuf, &len) < 0)
	  return -1;

	if (redis_read_sep(ctx, &rbuf) < 0)
	  return -1;
        
	tptr  = ptr[crec];
	tlen  = len;

	if (tlen > size[crec])
	  tlen = size[crec];

	size[crec] = tlen;

	while (tlen)
	{
          redis_buf_fill(ctx, &rbuf);

	  tsize = redis_buf_consume(&rbuf, (char *)tptr, tlen);

	  if (tsize < 0)
	    return -1;

          tlen -= tsize;
	  len  -= tsize;
	}

	tlen = len;

	while (tlen)
	{
          redis_buf_fill(ctx, &rbuf);

	  tsize = redis_buf_consume(&rbuf, buf, sizeof(buf));

	  if (tsize < 0)
	    return -1;

          tlen -= tsize;
	  len  -= tsize;
	}

	if (redis_read_sep(ctx, &rbuf) < 0)
	  return -1;

        nrec--;
	crec++;
        
	break;
      }
    }
  }

  return crec;
}

int redis_get_keys(redis_ctx_t *ctx, char *pat, int plen, void *ptr[], size_t size[], int n)
{
  int          rc;
  int          rem;
  int          len;
  int          rlen;
  int          parsed;
  char        *sp;
  char         buf[REDIS_KEY_LEN * 2];
  int          to_read;
  int          copied;
  struct iovec iovec[5];

  //printf("redis_get : key [%.*s] %d\n", klen, key, klen); 

  iovec[0].iov_base = "*2\r\n";
  iovec[0].iov_len  = strlen(iovec[0].iov_base);

  iovec[1].iov_base = "$4\r\nKEYS\r\n";  /* GET - 3 char */
  iovec[1].iov_len  = strlen(iovec[1].iov_base);

  sprintf(buf, "$%d\r\n", plen);
  iovec[2].iov_base = buf;
  iovec[2].iov_len  = strlen(buf);;

  iovec[3].iov_base = pat;
  iovec[3].iov_len  = plen;

  iovec[4].iov_base = "\r\n";
  iovec[4].iov_len  = 2;

  if ((rc = writev(ctx->sfd, iovec, sizeof(iovec)/sizeof(iovec[0]))) < 0)
    return -1;

  rc = redis_read_data(ctx, ptr, size, n);

  return rc;
}

int redis_set(redis_ctx_t *ctx, char *key, int klen, void *val, int vlen)
{
  int          rc;
  char        *sp;
  char         buf[64];
  char         buf2[64];
  struct iovec iovec[8];

  //printf("redis_set : key [%.*s] %d [%.*s] %d\n", klen, key, klen, vlen, val, vlen);

  iovec[0].iov_base = "*3\r\n";
  iovec[0].iov_len  = strlen(iovec[0].iov_base);

  iovec[1].iov_base = "$3\r\nSET\r\n";  /* SET - 3 char */
  iovec[1].iov_len  = strlen(iovec[1].iov_base);

  sprintf(buf, "$%d\r\n", klen);
  iovec[2].iov_base = buf;
  iovec[2].iov_len  = strlen(buf);

  iovec[3].iov_base = key;
  iovec[3].iov_len  = klen;

  iovec[4].iov_base = "\r\n";
  iovec[4].iov_len  = 2;

  sprintf(buf2, "$%d\r\n", vlen);
  iovec[5].iov_base = buf2;
  iovec[5].iov_len  = strlen(buf2);

  iovec[6].iov_base = val;
  iovec[6].iov_len  = vlen;

  iovec[7].iov_base = "\r\n";
  iovec[7].iov_len  = 2;

  if ((rc = writev(ctx->sfd, iovec, sizeof(iovec)/sizeof(iovec[0]))) < 0)
    return rc;

  if ((rc = read(ctx->sfd, buf, sizeof(buf))) < 0)
    return 0;

  if (memcmp(buf, REDIS_OK_STR, REDIS_OK_LEN) != 0)
    return -1;

  return 0;
}

int redis_del(redis_ctx_t *ctx, char *key, int klen)
{
  int          rc;
  char        *sp;
  char         buf[64];
  struct iovec iovec[5];

  //printf("redis_set : key [%.*s] %d [%.*s] %d\n", klen, key, klen, vlen, val, vlen);

  iovec[0].iov_base = "*2\r\n";
  iovec[0].iov_len  = strlen(iovec[0].iov_base);

  iovec[1].iov_base = "$3\r\nDEL\r\n";  /* SET - 3 char */
  iovec[1].iov_len  = strlen(iovec[1].iov_base);

  sprintf(buf, "$%d\r\n", klen);
  iovec[2].iov_base = buf;
  iovec[2].iov_len  = strlen(buf);

  iovec[3].iov_base = key;
  iovec[3].iov_len  = klen;

  iovec[4].iov_base = "\r\n";
  iovec[4].iov_len  = 2;

  if ((rc = writev(ctx->sfd, iovec, sizeof(iovec)/sizeof(iovec[0]))) < 0)
    return rc;

  if ((rc = read(ctx->sfd, buf, sizeof(buf))) < 0)
    return 0;

  if (memcmp(buf, REDIS_OK_STR, REDIS_OK_LEN) != 0)
    return -1;

  return 0;
}

int redis_close(redis_ctx_t *ctx)
{
  int ret;

  ret = close(ctx->sfd);

  if (ret < 0)
    ret;

  ctx->sfd = -1;

  return ret;
}

#ifdef TEST
int main()
{
  char         out[4096];
  int          len;
  char        *key1 = "xyz";
  char        *key2 = "xyz2";
  char        *val1 = "1234";
  char        *val2 = "2345";
  int          klen1 = strlen(key1);
  int          vlen1 = strlen(val1);
  int          vlen2 = strlen(val2);
  redis_ctx_t  ctx;

  if (redis_connect(&ctx, NULL, 0) < 0)
  {
    printf("redis_connect failed\n");
    return 0;
  }

  if (redis_set(&ctx, key1, klen1, val1, vlen1) < 0)
  {
    printf("redis_get failed\n");
    return 0;
  }
  printf("main : set [%.*s] = [%.*s] %d\n", klen1, key1, vlen1, val1, vlen1);

  if (redis_get(&ctx, key1, klen1, out, &len) < 0)
  {
    printf("redis_get failed\n");
    return 0;
  }
  printf("main : get [%.*s] = [%.*s] %d\n", klen1, key1, len, out, len);

  if (redis_set(&ctx, key1, klen1, val2, vlen2) < 0)
  {
    printf("redis_get failed\n");
    return 0;
  }
  printf("main : set [%.*s] = [%.*s] %d\n", klen1, key1, vlen2, val2, vlen2);

  if (redis_get(&ctx, key1, klen1, out, &len) < 0)
  {
    printf("redis_get failed\n");
    return 0;
  }

  printf("main : get [%.*s] = [%.*s] %d\n", klen1, key1, len, out, len);

  redis_close(&ctx);
}
int key_test()
  {
    void  *tdata[16];
    size_t tsize[16];
    char *pat = "/[^/]*@fobj";
    char   tbuf[16][512];
    int ind;

    for (ind = 0; ind < 16; ind++)
    {
      tdata[ind] = tbuf[ind];
      tsize[ind] = 512;
    }

  // redis_get(&redis_ctx, "key_1_12_abcd", strlen("key_1_12_abcd"), data, sizeof(data));
    ret = redis_get_keys(&redis_ctx, pat, strlen(pat), tdata, tsize, 16);

    printf("Read keys = %d\n", ret);

    for (ind = 0; ind < ret; ind++)
    {
      printf("key[%d] : [%s]\n", ind, (char *)tdata[ind]);
    }

  }
#endif














