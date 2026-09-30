/* 主机测试：仅驱动实际 AO_Store 状态处理函数，不模拟 QV 调度或中断。 */
#ifndef TEST_QPC_H
#define TEST_QPC_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint16_t QSignal;
typedef uint8_t QState;
typedef struct { QSignal sig; uint8_t poolId_, refCtr_; } QEvt;
typedef QState (*QStateHandler)(void *, QEvt const *);
typedef struct { QStateHandler handler; } QActive;
typedef struct { unsigned unused; } QTimeEvt;
enum { Q_ENTRY_SIG = 1, Q_USER_SIG = 4 };
#define Q_DEFINE_THIS_MODULE(name)
#define Q_STATE_CAST(handler) ((QStateHandler)(handler))
#define Q_TRAN(target) (me->super.handler = Q_STATE_CAST(target), (QState)0)
#define Q_HANDLED() ((QState)0)
#define Q_SUPER(target) ((QState)0)
#define Q_DIM(array) (sizeof(array)/sizeof((array)[0]))
static inline void QActive_ctor(QActive *ao, QStateHandler initial) { ao->handler = initial; }
static inline void QTimeEvt_ctorX(QTimeEvt *t, QActive *ao, unsigned sig, unsigned rate) {
    (void)t; (void)ao; (void)sig; (void)rate;
}
static inline void QTimeEvt_armX(QTimeEvt *t, unsigned first, unsigned period) { (void)t; (void)first; (void)period; }
static inline bool testPost(QActive *ao, QEvt const *e) { (void)ao; (void)e; return true; }
#define QACTIVE_POST_X(ao,e,margin,sender) testPost(ao,e)
static inline void testStart(QActive *ao, QEvt const **queue) {
    QEvt const entry = {Q_ENTRY_SIG,0U,0U}; (void)queue;
    (void)ao->handler(ao, NULL); (void)ao->handler(ao, &entry);
}
#define QACTIVE_START(ao,prio,queue,len,stack,size,event) testStart(ao,queue)
#endif
