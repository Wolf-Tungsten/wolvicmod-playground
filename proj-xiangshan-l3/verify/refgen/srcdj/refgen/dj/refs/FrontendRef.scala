package refgen.dj.refs

import chisel3._
import dongjiang.frontend.Frontend

// Frontend 参考（P3 步骤 5.4，真实配置，共享 DjConfig）。
object FrontendRef {
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("Frontend", () => {
      implicit val p: org.chipsalliance.cde.config.Parameters = DjConfig()
      new Frontend(isTop = true)
    })
  )
}
