package refgen.dj.refs

import chisel3._
import dongjiang.directory.Directory
import dongjiang.data.DataBlock

// Directory 参考（真实配置，共享 DjConfig，见 docs/dongjiang-semantics.md §1）。
object DirectoryRef {
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("Directory", () => {
      implicit val p: org.chipsalliance.cde.config.Parameters = DjConfig()
      new Directory(isTop = true)
    })
  )
}

// DataBlock 参考（P3 步骤 5.2）。
object DataBlockRef {
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("DataBlock", () => {
      implicit val p: org.chipsalliance.cde.config.Parameters = DjConfig()
      new DataBlock(isTop = true)
    })
  )
}
