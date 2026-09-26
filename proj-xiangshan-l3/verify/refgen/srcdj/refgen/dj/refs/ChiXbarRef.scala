package refgen.dj.refs

import chisel3._
import dongjiang.ChiXbar

// ChiXbar 参考（P3 步骤 5.5，真实配置，共享 DjConfig：
// nrIcn=1、nrDirBank=2、hasHPR=false、hasBBN=false）。
object ChiXbarRef {
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("ChiXbar", () => {
      implicit val p: org.chipsalliance.cde.config.Parameters = DjConfig()
      new ChiXbar
    })
  )
}
