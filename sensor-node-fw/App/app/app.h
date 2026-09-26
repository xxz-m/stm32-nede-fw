/*============================================================================
* app.h / app.c —— App 层装配点
*
* 层次定位：App 层唯一的「装配」入口，负责把各 AO 构造并启动起来。
*
* 为什么要这一层（整改项 1）：原先由 BSP 的 BSP_start() 调用 Node_ctor() 与
* QACTIVE_START()，形成 BSP -> App 的反向依赖，BSP 也无法脱离本应用复用。
* 现在装配点上移到 App 层：
*
*   main(): QF_init() -> BSP_init() -> App_start() -> QF_run()
*
* BSP 只提供硬件相关的初始化（BSP_init），不再认识任何 AO。
*===========================================================================*/
#ifndef APP_H
#define APP_H

/*! 构造并启动本应用的全部活动对象。
* @note 必须在 QF_init() 与 BSP_init() 之后、QF_run() 之前调用。
*/
void App_start(void);

#endif /* APP_H */
