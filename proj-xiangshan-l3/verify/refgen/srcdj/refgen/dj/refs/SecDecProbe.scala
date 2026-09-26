package refgen.dj.refs

import chisel3._
import dongjiang.frontend.SecDec
import dongjiang.frontend.decode.{Decode, StateInst}

// SecDec 行为探针：ci/si 作为运行时输入，直接观察真实 RTL
// Decode.secondDec((ci,0,0,0), si) 的 list(1) 结果。
// 用于裁决译码表 .inc 与 RTL 的一致性争议（PriorityEncoder 方向/行内容）。
class SecDecProbe extends Module {
  override def resetType: Module.ResetType.Type = Module.ResetType.Asynchronous
  implicit val p: org.chipsalliance.cde.config.Parameters = DjConfig()
  val io = IO(new Bundle {
    val ci  = Input(UInt(6.W))
    val si  = Input(UInt(5.W))
    val out = Output(UInt(3.W))
  })
  val sec  = Module(new SecDec)
  val list = Decode.listInit
  list(0) := io.ci
  sec.io.in   := list
  sec.io.inst := io.si.asTypeOf(new StateInst)
  io.out      := sec.io.out(1)
}

object SecDecProbeRef {
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("SecDecProbe", () => new SecDecProbe)
  )
}
