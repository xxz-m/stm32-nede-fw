/*============================================================================
* svc_filter.c —— 服务层：数字滤波实现
*
* 层次定位：Service 层。无 I/O、无框架依赖（不得 #include "qpc.h"）。
*===========================================================================*/
#include "svc_filter.h"

/*--------------------------------------------------------------------------*/
int32_t SVC_Filter_Median3(int32_t a, int32_t b, int32_t c) {
    int32_t t;

    /* 三步比较网络：排序后取中间值 */
    if (a > b) { t = a; a = b; b = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (a > b) { b = a; }

    return b;
}

/*--------------------------------------------------------------------------*/
bool SVC_Filter_MavgInit(SvcMavg *me, uint8_t len) {
    uint8_t i;

    if ((me == (SvcMavg *)0) || (len == 0U) || (len > SVC_FILTER_MAVG_MAX)) {
        return false;
    }

    for (i = 0U; i < SVC_FILTER_MAVG_MAX; ++i) {
        me->buf[i] = 0;
    }
    me->sum    = 0;
    me->len    = len;
    me->idx    = 0U;
    me->filled = 0U;

    return true;
}

/*--------------------------------------------------------------------------*/
int32_t SVC_Filter_MavgPush(SvcMavg *me, int32_t sample) {
    if ((me == (SvcMavg *)0) || (me->len == 0U)) {
        return sample; /* 未初始化：退化为直通，不产生除零 */
    }

    if (me->filled == me->len) {
        me->sum -= me->buf[me->idx];   /* 窗口已满：移除最旧样本 */
    }
    else {
        ++me->filled;
    }

    me->buf[me->idx] = sample;
    me->sum += sample;
    me->idx = (uint8_t)((me->idx + 1U) % me->len);

    return (me->sum / (int32_t)me->filled); /* 整数除法，向零截断 */
}
