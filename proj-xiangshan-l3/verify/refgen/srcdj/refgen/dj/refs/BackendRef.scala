package refgen.dj.refs

import chisel3._
import dongjiang.backend.Backend

// Backend 参考（P3 步骤 5.3，真实配置，共享 DjConfig）。
object BackendRef {
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("Backend", () => {
      implicit val p: org.chipsalliance.cde.config.Parameters = DjConfig()
      new Backend(isTop = true)
    })
  )
}
