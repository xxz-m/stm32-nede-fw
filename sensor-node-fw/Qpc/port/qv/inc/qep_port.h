/**
* @file
* @brief QEP/C port, ARM-Keil compiler 5
* @ingroup ports
* @cond
******************************************************************************
* Last updated for version 6.8.0
* Last updated on  2020-01-25
*
*                    Q u a n t u m  L e a P s
*                    ------------------------
*                    Modern Embedded Software
*
* Copyright (C) 2005-2019 Quantum Leaps, LLC. All rights reserved.
*
* This program is open source software: you can redistribute it and/or
* modify it under the terms of the GNU General Public License as published
* by the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* Alternatively, this program may be distributed and modified under the
* terms of Quantum Leaps commercial licenses, which expressly supersede
* the GNU General Public License and are specifically designed for
* licensees interested in retaining the proprietary status of their code.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <www.gnu.org/licenses>.
*
* Contact information:
* <www.state-machine.com/licensing>
* <info@state-machine.com>
******************************************************************************
* @endcond
*/
#ifndef QEP_PORT_H
#define QEP_PORT_H

/*! 无返回函数说明符
*
* NOTE（为接入本工程而修改）：ARM Compiler 5（armcc）使用微软风格的
* __declspec(noreturn)，而 ARM Compiler 6（armclang）、GCC 与 IAR 使用
* __attribute__((noreturn))。armclang 会定义 __clang__ / __GNUC__，因此需要
* 把上游原来的单一定义改成按编译器条件选择。
*/
#if defined(__clang__) || defined(__GNUC__)
    #define Q_NORETURN   __attribute__((noreturn)) void
#else
    #define Q_NORETURN   __declspec(noreturn) void
#endif

#include <stdint.h>  /* Exact-width types. WG14/N843 C99 Standard */
#include <stdbool.h> /* Boolean type.      WG14/N843 C99 Standard */

#include "qep.h"     /* QEP platform-independent public interface */

#endif /* QEP_PORT_H */
