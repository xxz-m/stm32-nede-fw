/*============================================================================
* bsp_i2c.c —— 板级支持层：I2C1 主机实现（非阻塞中断传输 + 总线恢复）
*
* 层次定位：BSP 层。唯一接触 I2C1 与 HAL_I2C 的地方。
*
* 数据通路（全程非阻塞）：
*   启动：BSP_i2cMemWriteIt() / BSP_i2cMemReadIt()
*           -> HAL_I2C_Mem_Write_IT() / HAL_I2C_Mem_Read_IT()   立即返回
*   结束：HAL_I2C_MemTxCpltCallback() / MemRxCpltCallback() / ErrorCallback()
*           -> 清 busy、记录状态、调用 App 注册的 sink（只投递事件）
*   推进：调用方每拍看 BSP_i2cIsBusy() / BSP_i2cLastStatus()，或在 sink 里唤醒
*
* 为什么不用阻塞式 HAL_I2C_Mem_Write()：
*   本工程是 QP/C 的 QV 协作式内核，AO 的处理函数不返回则整个系统停摆
*   （见 AGENTS.md 5.2）。一次 16 B 页写在 100 kHz 下约 1.5 ms，加上 EEPROM 的
*   内部写周期后总等待可达几十毫秒，绝不能在 AO 里原地等。因此这里只提供
*   「发起」与「查询」两个动作，等待交给状态机。
*
* 注意：本文件不得包含 App/Service/Driver 的头文件（依赖单向向下）。
*===========================================================================*/
#include "main.h"
#include "i2c.h"        /* hi2c1 与 MX_I2C1_Init()（CubeMX 生成） */
#include "bsp.h"

/*--------------------------------------------------------------------------*/
/* 内部状态                                                                  */
/*--------------------------------------------------------------------------*/
static volatile bool         l_busy;      /* 有一次中断传输在飞              */
static volatile bsp_status_t l_last;      /* 最近一次传输的结束状态          */
static volatile uint32_t     l_xfers;     /* 成功完成的传输次数（诊断）      */
static volatile uint32_t     l_errors;    /* 失败的传输次数（诊断）          */
static uint32_t              l_recovers;  /* 总线恢复执行次数（诊断）        */
static BspI2cSink            l_sink;      /* App 注册的完成回调              */

/*--------------------------------------------------------------------------*/
/* HAL 回调（覆盖 stm32f1xx_hal_i2c.c 里的 __weak 定义）                      */
/*--------------------------------------------------------------------------*/
/**
* @brief 中断传输的统一收尾：清 busy、记状态、通知上层。
*
* NOTE (防御)：只有本模块发起过传输（l_busy 为真）时才认这个回调。HAL 在阻塞式
* 接口（如 HAL_I2C_IsDeviceReady 用到的轮询路径）里也可能调用 ErrorCallback，
* 那种情况不是我们的传输，直接忽略，避免把扫描的失败误记成一次总线故障。
*/
static void i2cFinish(I2C_HandleTypeDef *hi2c, bsp_status_t st) {
    BspI2cSink sink;

    if ((hi2c == (I2C_HandleTypeDef *)0) || (hi2c->Instance != I2C1)) {
        return;
    }
    if (!l_busy) {
        return;
    }

    l_busy = false;
    l_last = st;
    if (st == BSP_OK) {
        ++l_xfers;
    }
    else {
        ++l_errors;
    }

    sink = l_sink;
    if (sink != (BspI2cSink)0) {
        sink();   /* 中断上下文：回调只允许投递事件 */
    }
}

/*..........................................................................*/
void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c) {
    i2cFinish(hi2c, BSP_OK);
}

/*..........................................................................*/
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c) {
    i2cFinish(hi2c, BSP_OK);
}

/*..........................................................................*/
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c) {
    i2cFinish(hi2c, BSP_ERR_IO);
}

/*--------------------------------------------------------------------------*/
/* 中断入口                                                                  */
/*--------------------------------------------------------------------------*/
/* NOTE (弱符号覆盖)：启动文件 startup_stm32f103xe.s 把这两个向量定义为弱符号，
* 这里给出强定义即完成接管。CubeMX 未在 .ioc 里使能 I2C1 中断，因此不会与
* Core/Src/stm32f1xx_it.c 重复——详见 bsp_i2c.h 的 NOTE。 */
void I2C1_EV_IRQHandler(void) {
    HAL_I2C_EV_IRQHandler(&hi2c1);
}

/*..........................................................................*/
void I2C1_ER_IRQHandler(void) {
    HAL_I2C_ER_IRQHandler(&hi2c1);
}

/*--------------------------------------------------------------------------*/
/* 宏辅助                                                                    */
/*--------------------------------------------------------------------------*/
/** @brief 粗略微秒延时。
*
* 只用于总线恢复时拉电平的间隔，精度不重要（I2C 对恢复脉冲只要求 1~10 µs 量级）。
* 72 MHz 下每次循环约 4 个周期，18 次循环约 1 µs。
*/
static void i2cDelayUs(uint32_t us) {
    volatile uint32_t n = us * 18U;

    while (n != 0U) {
        --n;
    }
}

/*--------------------------------------------------------------------------*/
/* 总线恢复                                                                  */
/*--------------------------------------------------------------------------*/
/**
* @brief 把 I2C1 复位、用 GPIO 手工发 9 个时钟 + STOP，再重新初始化外设。
*
* 为什么需要：主控在传输过程中被复位（或从机供电抖动）时，从机可能正把 SDA 拉低
* 等着第 9 个时钟，于是总线永久处于 BUSY，之后每次传输都超时。手册给的标准补救
* 就是从机侧补足时钟让它把剩余位吐完，再补一个 STOP。这是唯一能不打电就恢复的手段。
*/
bsp_status_t BSP_i2cBusRecover(void) {
    GPIO_InitTypeDef gpio = {0};
    uint8_t i;

    if (hi2c1.Instance != I2C1) {
        return BSP_ERR_IO;
    }

    l_busy = false;

    /* 先关外设并释放 PB6/PB7（HAL_I2C_MspDeInit 会关时钟并 DeInit 这两个脚） */
    (void)HAL_I2C_DeInit(&hi2c1);

    /* 两个脚都配成开漏输出，电平靠外部上拉（PB6/PB7 是 I2C 的正式引脚，
    * 板子上应有 4.7 k 上拉；没有上拉时总线本来也跑不通）。 */
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);   /* SDA 释放为高 */

    for (i = 0U; i < 9U; ++i) {                           /* 9 个时钟 */
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
        i2cDelayUs(5U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
        i2cDelayUs(5U);
    }

    /* STOP 时序：SCL 低 → SDA 低 → SCL 高 → SDA 高 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
    i2cDelayUs(5U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);
    i2cDelayUs(5U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    i2cDelayUs(5U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);
    i2cDelayUs(5U);

    /* 重新初始化外设（HAL_I2C_MspInit 会把 PB6/PB7 改回复用开漏并开时钟） */
    MX_I2C1_Init();

    l_last = BSP_ERR_IO;
    ++l_recovers;
    return BSP_OK;
}

/*--------------------------------------------------------------------------*/
/* 初始化                                                                    */
/*--------------------------------------------------------------------------*/
bsp_status_t BSP_i2cInit(void) {
    bool needRecover = false;

    if (hi2c1.Instance != I2C1) {
        BSP_printLine("i2c    : I2C1 not initialised (MX_I2C1_Init missing)");
        return BSP_ERR_IO;
    }

    l_busy = false;
    l_last = BSP_OK;

    /* 中断使能与优先级。NOTE (顺序)：QV_init() 会把所有中断优先级刷成 3，本函数
    * 在其之后运行，所以这里设的 5 会保留；5 >= QF_AWARE_ISR_CMSIS_PRI(3)，
    * 因此 I2C 中断里允许调用 QACTIVE_POST_X()。 */
    HAL_NVIC_SetPriority(I2C1_EV_IRQn, BSP_I2C_IRQ_PRIO, 0U);
    HAL_NVIC_SetPriority(I2C1_ER_IRQn, BSP_I2C_IRQ_PRIO, 0U);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);

    /* BUSY 卡死或外设不在 READY：先恢复总线，否则之后每次传输都会走到超时。
    * NOTE (为什么在初始化里就查)：上一次复位如果正好发生在一次传输中间，
    * 从机会一直等时钟；不在这里清掉，现场看到的现象是「I2C 永远超时」。 */
    if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) != RESET) {
        needRecover = true;
    }
    else if (hi2c1.State != HAL_I2C_STATE_READY) {
        needRecover = true;
    }

    BSP_print("i2c    : I2C1 PB6/PB7 ");
    BSP_printU32(hi2c1.Init.ClockSpeed);
    BSP_printLine(" Hz  (param store)");

    if (needRecover) {
        BSP_printLine("i2c    : bus busy after reset -> recovering");
        (void)BSP_i2cBusRecover();
    }

    return BSP_OK;
}

/*..........................................................................*/
void BSP_i2cSetSink(BspI2cSink sink) {
    l_sink = sink;
}

/*--------------------------------------------------------------------------*/
/* 中断传输                                                                  */
/*--------------------------------------------------------------------------*/
bsp_status_t BSP_i2cMemWriteIt(uint8_t devAddr8, uint8_t memAddr,
                               uint8_t const *buf, uint16_t len) {
    HAL_StatusTypeDef st;

    if ((buf == (uint8_t const *)0) || (len == 0U) || (len > 255U)) {
        return BSP_ERR_PARAM;
    }
    if (l_busy || (hi2c1.State != HAL_I2C_STATE_READY)) {
        return BSP_ERR_BUSY;
    }

    l_last = BSP_ERR_TIMEOUT;   /* 若回调始终不来，调用方按超时处理 */
    l_busy = true;

    /* NOTE (去 const)：HAL 的 IT 接口签名要 uint8_t*，但它只读该缓冲。 */
    st = HAL_I2C_Mem_Write_IT(&hi2c1, (uint16_t)devAddr8, (uint16_t)memAddr,
                              I2C_MEMADD_SIZE_8BIT, (uint8_t *)(void *)buf, len);
    if (st != HAL_OK) {
        l_busy = false;
        ++l_errors;
        return (st == HAL_BUSY) ? BSP_ERR_BUSY : BSP_ERR_IO;
    }

    return BSP_OK;
}

/*..........................................................................*/
bsp_status_t BSP_i2cMemReadIt(uint8_t devAddr8, uint8_t memAddr,
                              uint8_t *buf, uint16_t len) {
    HAL_StatusTypeDef st;

    if ((buf == (uint8_t *)0) || (len == 0U) || (len > 255U)) {
        return BSP_ERR_PARAM;
    }
    if (l_busy || (hi2c1.State != HAL_I2C_STATE_READY)) {
        return BSP_ERR_BUSY;
    }

    l_last = BSP_ERR_TIMEOUT;
    l_busy = true;

    st = HAL_I2C_Mem_Read_IT(&hi2c1, (uint16_t)devAddr8, (uint16_t)memAddr,
                             I2C_MEMADD_SIZE_8BIT, buf, len);
    if (st != HAL_OK) {
        l_busy = false;
        ++l_errors;
        return (st == HAL_BUSY) ? BSP_ERR_BUSY : BSP_ERR_IO;
    }

    return BSP_OK;
}

/*..........................................................................*/
bool BSP_i2cIsBusy(void) {
    return l_busy;
}

/*..........................................................................*/
bsp_status_t BSP_i2cLastStatus(void) {
    return l_last;
}

/*--------------------------------------------------------------------------*/
/* 诊断                                                                      */
/*--------------------------------------------------------------------------*/
bsp_status_t BSP_i2cProbe(uint8_t devAddr8, uint32_t timeoutMs) {
    HAL_StatusTypeDef st;

    if ((devAddr8 == 0U) || (timeoutMs == 0U)) {
        return BSP_ERR_PARAM;
    }
    if (l_busy || (hi2c1.State != HAL_I2C_STATE_READY)) {
        return BSP_ERR_BUSY;
    }

    /* NOTE (阻塞预算)：Trials=1、timeoutMs 由调用方给 1~2 ms，实际耗时通常是
    * 一个地址字节的时间（100 kHz 下约 90 µs）；超时上限保证了最坏情况有界。
    * 这是本模块唯一的阻塞调用，只服务 I2C 扫描。 */
    st = HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)devAddr8, 1U, timeoutMs);

    return (st == HAL_OK) ? BSP_OK : BSP_ERR_TIMEOUT;
}

/*..........................................................................*/
uint32_t BSP_i2cErrorCount(void) {
    return l_errors;
}

/*..........................................................................*/
uint32_t BSP_i2cRecoverCount(void) {
    return l_recovers;
}

/*..........................................................................*/
uint32_t BSP_i2cXferCount(void) {
    return l_xfers;
}
