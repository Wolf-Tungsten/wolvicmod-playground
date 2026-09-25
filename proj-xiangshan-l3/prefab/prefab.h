#pragma once

// 项目侧预制菜：XiangShan 生态（xs-utils / dongjiang）特有的时序元件，
// 命名空间 zj::prefab。这些是"用 wolvicmod 框架写的普通库模块"，与参考 RTL
// 拍级对齐；框架通用的 chisel3 标准库元件（Dec 通道约定、Queue、FixedArb、
// RRArb、ValidPipe）在 wolvicmod 侧（<wolvicmod/prefab/prefab.h>），本库直接
// 复用（Dec/FixedArb/ValidPipe 经 using 引入）。
//
//   fastq.h —— FastQueue<T,N,NoX>      ↔ xs-utils queue/FastQueue.scala
//   xsarb.h —— VipArb<T,N>             ↔ xs-utils arb/VipArbiter.scala
//              QosRRArb/QosFixedArb     ↔ dongjiang utils/FastArb.scala
//              Alloc<T,N>              ↔ dongjiang utils/Alloc.scala
//   sram.h  —— SpSram/DpSram           ↔ xs-utils sram/Single|DualPortSramTemplate.scala

#include "prefab/fastq.h"
#include "prefab/sram.h"
#include "prefab/xsarb.h"
