package refgen.dj.refs

import org.chipsalliance.cde.config.{Config, Parameters}
import zhujiang.{ZJParameters, ZJParametersKey}
import top.ZhuJiangNoCTopology
import xs.utils.debug.{HardwareAssertionKey, HwaParams}
import xs.utils.perf.{LogUtilsOptions, LogUtilsOptionsKey, PerfCounterOptions, PerfCounterOptionsKey, XSPerfLevel}

// DongJiang 参考共享配置：kunminghu-v3 DefaultConfig + LLC=ZhuJiang 单核真实配置
// （DefaultConfig = ZhuJiangConfig("32MB", 16) → cacheSizeInB=32MB；
//   bank=2 → 每 DongJiang llc 16MB(8192 组/路 16/tag 27)，详见 docs/dongjiang-semantics.md §1）。
// 配置键对齐 TestTopZhuJiang：断言关闭、perf 关闭（generate 干净的纯 RTL）。
object DjConfig {
  def apply(): Parameters = new Config((site, here, up) => {
    case ZJParametersKey =>
      ZhuJiangNoCTopology(1, ZJParameters().copy(cacheSizeInB = 32 * 1024 * 1024, cacheWays = 16), 256)
    case HardwareAssertionKey => HwaParams(enable = false)
    case LogUtilsOptionsKey   => LogUtilsOptions(false, false, false)
    case PerfCounterOptionsKey =>
      PerfCounterOptions(false, false, XSPerfLevel.withName("VERBOSE"), 0)
  })
}
