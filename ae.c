/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
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
//居然嵌入一个event. 看来只要是起服务的代码都内嵌一个event事件驱动. 294行就看到核心还是select函数!
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

#include "ae.h"
#include "zmalloc.h"
//  结构是最上层是aeEventLoop  然后里面有两个链表一个fileevent, 一个timeevent. 下面几个函数是eventloop的操作.
aeEventLoop *aeCreateEventLoop(void)
{
    aeEventLoop *eventLoop;

    eventLoop = zmalloc(sizeof(*eventLoop));
    if (!eventLoop)
        return NULL;
    eventLoop->fileEventHead = NULL;
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    return eventLoop;
}

void aeDeleteEventLoop(aeEventLoop *eventLoop)
{
    zfree(eventLoop);
}

void aeStop(aeEventLoop *eventLoop)
{
    eventLoop->stop = 1;
}

//下面是fileevent的操作.
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
                      aeFileProc *proc, void *clientData,
                      aeEventFinalizerProc *finalizerProc)
{
    aeFileEvent *fe;

    fe = zmalloc(sizeof(*fe));
    if (fe == NULL)
        return AE_ERR;
    fe->fd = fd;
    fe->mask = mask;
    fe->fileProc = proc;
    fe->finalizerProc = finalizerProc;
    fe->clientData = clientData;
    fe->next = eventLoop->fileEventHead;
    eventLoop->fileEventHead = fe;
    return AE_OK;
}

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{ //删除eventloop中关联fd, 且mask符合的.把他删除掉.   mak=/* one of AE_(READABLE|WRITABLE|EXCEPTION) */
    aeFileEvent *fe, *prev = NULL;

    fe = eventLoop->fileEventHead;
    while (fe)
    {
        if (fe->fd == fd && fe->mask == mask) //
        {
            if (prev == NULL)
                eventLoop->fileEventHead = fe->next;
            else
                prev->next = fe->next;
            if (fe->finalizerProc) //如果有析构处理,就进行析构处理函数.
                fe->finalizerProc(eventLoop, fe->clientData);
            zfree(fe);
            return; //找到他就删除即可.因为只会有一个匹配要删除的.不会更多.
        }
        prev = fe;
        fe = fe->next;
    }
}

//下面处理时间事件.
static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec / 1000;
}

static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms)
{
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + milliseconds / 1000;
    when_ms = cur_ms + milliseconds % 1000;
    if (when_ms >= 1000) //浩渺换算成秒.
    {
        when_sec++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}
//创建时间事件到aeEventLoop这个变量里面.
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData,
                            aeEventFinalizerProc *finalizerProc)
{
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL)
        return AE_ERR;
    te->id = id;
    aeAddMillisecondsToNow(milliseconds, &te->when_sec, &te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te, *prev = NULL;

    te = eventLoop->timeEventHead;
    while (te)
    {
        if (te->id == id)
        {
            if (prev == NULL)
                eventLoop->timeEventHead = te->next;
            else
                prev->next = te->next;
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            zfree(te);
            return AE_OK;
        }
        prev = te;
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted. */
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while (te)
    {
        if (!nearest || te->when_sec < nearest->when_sec ||
            (te->when_sec == nearest->when_sec &&
             te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurrs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int maxfd = 0, numfd = 0, processed = 0;
    fd_set rfds, wfds, efds;
    aeFileEvent *fe = eventLoop->fileEventHead;
    aeTimeEvent *te;
    long long maxId;
    AE_NOTUSED(flags);

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS))
        return 0;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    /* Check file events */ // fd_set进去
    if (flags & AE_FILE_EVENTS)
    {
        while (fe != NULL)
        {
            if (fe->mask & AE_READABLE)
                FD_SET(fe->fd, &rfds);
            if (fe->mask & AE_WRITABLE)
                FD_SET(fe->fd, &wfds);
            if (fe->mask & AE_EXCEPTION)
                FD_SET(fe->fd, &efds);
            if (maxfd < fe->fd)
                maxfd = fe->fd;
            numfd++;
            fe = fe->next;
        }
    }
    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (numfd || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT)))
    { //((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT)) 这段是时间事件是否存在的判断.
        int retval;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT)) // &是按位运算, &&是逻辑运算. 这段表示付过falgs有时间事件并且是需要等待的.情况下我们就进行触发.
            shortest = aeSearchNearestTimer(eventLoop);
        if (shortest)
        {
            long now_sec, now_ms;

            /* Calculate the time missing for the nearest
             * timer to fire. */
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec; // tvp是时间间隔的意思.
            if (shortest->when_ms < now_ms)             //减法不够就借位.
            {
                tvp->tv_usec = ((shortest->when_ms + 1000) - now_ms) * 1000;
                tvp->tv_sec--;
            }
            else
            {
                tvp->tv_usec = (shortest->when_ms - now_ms) * 1000;
            }
        }
        else
        {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to se the timeout
             * to zero */
            if (flags & AE_DONT_WAIT)
            {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            }
            else
            {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }

        retval = select(maxfd + 1, &rfds, &wfds, &efds, tvp);
        if (retval > 0)
        {
            fe = eventLoop->fileEventHead;
            while (fe != NULL)
            {
                int fd = (int)fe->fd;
                // 使用FD_ISSET运算找出来触发的事件是什么.
                if ((fe->mask & AE_READABLE && FD_ISSET(fd, &rfds)) ||
                    (fe->mask & AE_WRITABLE && FD_ISSET(fd, &wfds)) ||
                    (fe->mask & AE_EXCEPTION && FD_ISSET(fd, &efds)))
                { //现在fd触发了.
                    int mask = 0;

                    if (fe->mask & AE_READABLE && FD_ISSET(fd, &rfds))
                        mask |= AE_READABLE;
                    if (fe->mask & AE_WRITABLE && FD_ISSET(fd, &wfds))
                        mask |= AE_WRITABLE;
                    if (fe->mask & AE_EXCEPTION && FD_ISSET(fd, &efds))
                        mask |= AE_EXCEPTION;
                    //看看fd都触发什么事件. 触发的放mask里面. 调用cb函数即可.
                    //这里面cb函数就是fileProc.也需要自己实现的.
                    fe->fileProc(eventLoop, fe->fd, fe->clientData, mask);
                    processed++;
                    /* After an event is processed our file event list
                     * may no longer be the same, so what we do
                     * is to clear the bit for this file descriptor and
                     * restart again from the head. */
                    fe = eventLoop->fileEventHead; // 因为fe会在下面else里面一直遍历链表.触发一次.我们就链表重新归头部重新扫描.
                    FD_CLR(fd, &rfds);             //清空标志.
                    FD_CLR(fd, &wfds);
                    FD_CLR(fd, &efds);
                }
                else
                {
                    fe = fe->next;
                }
            }
        }
    }
    /* Check time events */
    if (flags & AE_TIME_EVENTS)
    {
        te = eventLoop->timeEventHead;
        maxId = eventLoop->timeEventNextId - 1;
        while (te) //时间时间就自己扫描来实现.不会到时间就返回而只会在超时时候再访问的话就报超时而已.
        {
            long now_sec, now_ms;
            long long id;

            if (te->id > maxId)
            {
                te = te->next;
                continue;
            }
            aeGetTime(&now_sec, &now_ms);
            if (now_sec > te->when_sec ||
                (now_sec == te->when_sec && now_ms >= te->when_ms))
            { //如果当前时间大于了时间截止时间.我们就触发处理函数.
                int retval;

                id = te->id;
                retval = te->timeProc(eventLoop, id, te->clientData);
                /* After an event is processed our time event list may
                 * no longer be the same, so we restart from head.
                 * Still we make sure to don't process events registered
                 * by event handlers itself in order to don't loop forever.
                 * To do so we saved the max ID we want to handle. */
                if (retval != AE_NOMORE)
                {                                                                //如果处理结果不是永远不要了.那么
                    aeAddMillisecondsToNow(retval, &te->when_sec, &te->when_ms); //这个函数现在的时间加上第一个变量,结果的秒和毫秒放入后面2个变量里面.也就是让触发函数加上一个时间然后继续等待下次的超时判断!
                }
                else
                {
                    aeDeleteTimeEvent(eventLoop, id);
                }
                te = eventLoop->timeEventHead; //每次处理完再回头部即可.
            }
            else
            {
                te = te->next;
            }
        }
    }
    return processed; /* return the number of processed file/time events */
}

/* Wait for millseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds)
{
    struct timeval tv;
    fd_set rfds, wfds, efds;
    int retmask = 0, retval;

    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    // 把监听文件的事件放入fd里面.
    if (mask & AE_READABLE)
        FD_SET(fd, &rfds);
    if (mask & AE_WRITABLE)
        FD_SET(fd, &wfds);
    if (mask & AE_EXCEPTION)
        FD_SET(fd, &efds); //核心调用下面select即可. 说白了整个函数就是select封装而已.超时是最后一个参数.
    if ((retval = select(fd + 1, &rfds, &wfds, &efds, &tv)) > 0)
    {
        if (FD_ISSET(fd, &rfds))
            retmask |= AE_READABLE;
        if (FD_ISSET(fd, &wfds))
            retmask |= AE_WRITABLE;
        if (FD_ISSET(fd, &efds))
            retmask |= AE_EXCEPTION; //跟上面一样把触发的retmask找出来返回即可.
        return retmask;
    }
    else
    {
        return retval;
    }
}

void aeMain(aeEventLoop *eventLoop)
{
    eventLoop->stop = 0;
    while (!eventLoop->stop) //主函数让服务死循环起来.一直监听.
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
}
