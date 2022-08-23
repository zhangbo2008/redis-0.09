/* Redis CLI (command line interface)
 *
 * Copyright (c) 2006-2009, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
// 最后是这2个核心文件. 一个是客户端. 就是redis-cli.c 一个是服务端 redis.c
//先看简单的客户端.
#include "fmacros.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "anet.h"
#include "sds.h"
#include "adlist.h"
#include "zmalloc.h"

#define REDIS_CMD_INLINE 1
#define REDIS_CMD_BULK 2

#define REDIS_NOTUSED(V) ((void)V)

static struct config
{
    char *hostip;
    int hostport;
} config;

struct redisCommand
{
    char *name;
    int arity;
    int flags;
};
//用一个数组来存所有的命令
static struct redisCommand cmdTable[] = {
    {"get", 2, REDIS_CMD_INLINE},
    {"set", 3, REDIS_CMD_BULK},
    {"setnx", 3, REDIS_CMD_BULK},
    {"del", 2, REDIS_CMD_INLINE},
    {"exists", 2, REDIS_CMD_INLINE},
    {"incr", 2, REDIS_CMD_INLINE},
    {"decr", 2, REDIS_CMD_INLINE},
    {"rpush", 3, REDIS_CMD_BULK},
    {"lpush", 3, REDIS_CMD_BULK},
    {"rpop", 2, REDIS_CMD_INLINE},
    {"lpop", 2, REDIS_CMD_INLINE},
    {"llen", 2, REDIS_CMD_INLINE},
    {"lindex", 3, REDIS_CMD_INLINE},
    {"lset", 4, REDIS_CMD_BULK},
    {"lrange", 4, REDIS_CMD_INLINE},
    {"ltrim", 4, REDIS_CMD_INLINE},
    {"lrem", 4, REDIS_CMD_BULK},
    {"sadd", 3, REDIS_CMD_BULK},
    {"srem", 3, REDIS_CMD_BULK},
    {"sismember", 3, REDIS_CMD_BULK},
    {"scard", 2, REDIS_CMD_INLINE},
    {"sinter", -2, REDIS_CMD_INLINE},
    {"sinterstore", -3, REDIS_CMD_INLINE},
    {"smembers", 2, REDIS_CMD_INLINE},
    {"incrby", 3, REDIS_CMD_INLINE},
    {"decrby", 3, REDIS_CMD_INLINE},
    {"randomkey", 1, REDIS_CMD_INLINE},
    {"select", 2, REDIS_CMD_INLINE},
    {"move", 3, REDIS_CMD_INLINE},
    {"rename", 3, REDIS_CMD_INLINE},
    {"renamenx", 3, REDIS_CMD_INLINE},
    {"keys", 2, REDIS_CMD_INLINE},
    {"dbsize", 1, REDIS_CMD_INLINE},
    {"ping", 1, REDIS_CMD_INLINE},
    {"echo", 2, REDIS_CMD_BULK},
    {"save", 1, REDIS_CMD_INLINE},
    {"bgsave", 1, REDIS_CMD_INLINE},
    {"shutdown", 1, REDIS_CMD_INLINE},
    {"lastsave", 1, REDIS_CMD_INLINE},
    {"type", 2, REDIS_CMD_INLINE},
    {"flushdb", 1, REDIS_CMD_INLINE},
    {"flushall", 1, REDIS_CMD_INLINE},
    {"sort", -2, REDIS_CMD_INLINE},
    {"info", 1, REDIS_CMD_INLINE},
    {"mget", -2, REDIS_CMD_INLINE},
    {"expire", 3, REDIS_CMD_INLINE},
    {NULL, 0, 0}};

static int cliReadReply(int fd);

//输入上面查询表里面一个name,返回他的具体文字信息.
static struct redisCommand *lookupCommand(char *name)
{
    int j = 0;
    while (cmdTable[j].name != NULL)
    {
        if (!strcasecmp(name, cmdTable[j].name))
            return &cmdTable[j];
        j++;
    }
    return NULL;
}
//主要就是调用anet.c里面的链接函数. 链接成功就返回fd. 否则失败了就 返回-1
static int cliConnect(void)
{
    char err[ANET_ERR_LEN];
    int fd;

    fd = anetTcpConnect(err, config.hostip, config.hostport); // 注意这个地方要用默认的模式也就是阻塞模式.来保证有信息了才读取fd.
    if (fd == ANET_ERR)
    {
        fprintf(stderr, "Connect: %s\n", err);
        return -1;
    }
    anetTcpNoDelay(NULL, fd);
    return fd;
}

static sds cliReadLine(int fd) //读一行最后不加回车
{
    sds line = sdsempty(); //来个空字符串.

    while (1)
    { //循环,每一次读fd里面一个字符.
        char c;
        ssize_t ret;

        ret = read(fd, &c, 1);
        if (ret == -1)
        {
            sdsfree(line);
            return NULL;
        }
        else if ((ret == 0) || (c == '\n'))
        { //读到\n,或者度到头了就break
            break;
        }
        else
        {
            line = sdscatlen(line, &c, 1); //把读到的内容cat起来.拼接成line了.
        }
    }
    return sdstrim(line, "\r\n");
}

static int cliReadSingleLineReply(int fd) //读一行最后价加个回车.
{
    sds reply = cliReadLine(fd);

    if (reply == NULL)
        return 1;
    printf("%s\n", reply);
    return 0;
}

static int cliReadBulkReply(int fd) //每次读很多行.
{
    sds replylen = cliReadLine(fd);
    char *reply, crlf[2];
    int bulklen;

    if (replylen == NULL)
        return 1;
    bulklen = atoi(replylen); //第一行先读取一个数字表示将要的数据大小.
    if (bulklen == -1)
    {
        sdsfree(replylen);
        printf("(nil)");
        return 0;
    }
    reply = zmalloc(bulklen);
    anetRead(fd, reply, bulklen);
    anetRead(fd, crlf, 2);
    if (bulklen && fwrite(reply, bulklen, 1, stdout) == 0)
    {                 //写入一个bulken大小的数据从reply到stdout
        zfree(reply); //如果没有写入数据就直接返回1即可
        return 1;
    }
    if (isatty(fileno(stdout)) && reply[bulklen - 1] != '\n')
        printf("\n"); //写入数据了,那么最后加\n
    zfree(reply);
    return 0;
}

static int cliReadMultiBulkReply(int fd)
{
    sds replylen = cliReadLine(fd);
    int elements, c = 1;

    if (replylen == NULL)
        return 1;
    elements = atoi(replylen);
    if (elements == -1)
    {
        sdsfree(replylen);
        printf("(nil)\n");
        return 0;
    }
    if (elements == 0)
    {
        printf("(empty list or set)\n");
    }
    while (elements--)
    {
        printf("%d. ", c);    //打印索引记号
        if (cliReadReply(fd)) //然后读一行.
            return 1;
        c++;
    }
    return 0;
}

static int cliReadReply(int fd) //读取返回的信息,根据标号,知道应该读多少.如何解析.
{
    char type;

    if (anetRead(fd, &type, 1) <= 0)
        exit(1);
    switch (type)
    {
    case '-':
        printf("(error) ");
        cliReadSingleLineReply(fd);
        return 1;
    case '+':
    case ':':
        return cliReadSingleLineReply(fd);
    case '$':
        return cliReadBulkReply(fd);
    case '*':
        return cliReadMultiBulkReply(fd);
    default:
        printf("protocol error, got '%c' as reply type byte\n", type);
        return 1;
    }
}

static int cliSendCommand(int argc, char **argv)
{
    struct redisCommand *rc = lookupCommand(argv[0]);
    int fd, j, retval = 0;
    sds cmd = sdsempty();

    if (!rc)
    {
        fprintf(stderr, "Unknown command '%s'\n", argv[0]);
        return 1;
    }
    // rc 是 lookup表里面的信息.对比参数数量.
    if ((rc->arity > 0 && argc != rc->arity) ||
        (rc->arity < 0 && argc < -rc->arity))
    {
        fprintf(stderr, "Wrong number of arguments for '%s'\n", rc->name);
        return 1;
    }
    if ((fd = cliConnect()) == -1)
        return 1;

    /* Build the command to send */
    for (j = 0; j < argc; j++)
    {
        if (j != 0)
            cmd = sdscat(cmd, " ");
        if (j == argc - 1 && rc->flags & REDIS_CMD_BULK)
        {
            cmd = sdscatprintf(cmd, "%d", sdslen(argv[j]));
        }
        else
        {
            cmd = sdscatlen(cmd, argv[j], sdslen(argv[j]));
        }
    }
    cmd = sdscat(cmd, "\r\n");
    if (rc->flags & REDIS_CMD_BULK)
    {
        cmd = sdscatlen(cmd, argv[argc - 1], sdslen(argv[argc - 1]));
        cmd = sdscat(cmd, "\r\n");
    }
    anetWrite(fd, cmd, sdslen(cmd)); //链接服务器,写入信息,等他返回.
    retval = cliReadReply(fd);//写完了读取即可.
    if (retval)
    {
        close(fd);
        return retval;
    }
    close(fd);
    return 0;
}

static int parseOptions(int argc, char **argv) // 读取信息写入config里面.返回剩余信息的索引位置.
{
    int i;

    for (i = 1; i < argc; i++)
    {
        int lastarg = i == argc - 1;

        if (!strcmp(argv[i], "-h") && !lastarg)
        {
            char *ip = zmalloc(32);
            if (anetResolve(NULL, argv[i + 1], ip) == ANET_ERR)
            {
                printf("Can't resolve %s\n", argv[i]);
                exit(1);
            }
            config.hostip = ip;
            i++;
        }
        else if (!strcmp(argv[i], "-p") && !lastarg)
        {
            config.hostport = atoi(argv[i + 1]);
            i++;
        }
        else
        {
            break;
        }
    }
    return i;
}

static sds readArgFromStdin(void)
{
    char buf[1024];
    sds arg = sdsempty();

    while (1)
    {
        int nread = read(fileno(stdin), buf, 1024);

        if (nread == 0)
            break;
        else if (nread == -1)
        {
            perror("Reading from standard input");
            exit(1);
        }
        arg = sdscatlen(arg, buf, nread);
    }
    return arg;
}

int main(int argc, char **argv)
{
    int firstarg, j;
    char **argvcopy;

    config.hostip = "127.0.0.1";
    config.hostport = 6379;

    firstarg = parseOptions(argc, argv);
    argc -= firstarg;
    argv += firstarg;

    /* Turn the plain C strings into Sds strings */ //剩余的参数写到argvcopy
    argvcopy = zmalloc(sizeof(char *) * argc + 1);
    for (j = 0; j < argc; j++)
        argvcopy[j] = sdsnew(argv[j]);

    /* Read the last argument from stdandard input */
    if (!isatty(fileno(stdin)))
    {
        sds lastarg = readArgFromStdin(); // 键盘输入命令.放入最后一个位置.
        argvcopy[argc] = lastarg;         // argvcopy每一个位置都存的是char* 也就是字符串.
        argc++;
    }

    if (argc < 1)
    {
        fprintf(stderr, "usage: redis-cli [-h host] [-p port] cmd arg1 arg2 arg3 ... argN\n");
        fprintf(stderr, "usage: echo \"argN\" | redis-cli [-h host] [-p port] cmd arg1 arg2 ... arg(N-1)\n");
        fprintf(stderr, "\nIf a pipe from standard input is detected this data is used as last argument.\n\n");
        fprintf(stderr, "example: cat /etc/passwd | redis-cli set my_passwd\n");
        fprintf(stderr, "example: redis-cli get my_passwd\n");
        exit(1);
    }

    return cliSendCommand(argc, argvcopy); //调用即可.
}
