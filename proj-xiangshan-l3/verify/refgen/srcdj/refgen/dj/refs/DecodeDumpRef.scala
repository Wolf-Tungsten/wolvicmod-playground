package refgen.dj.refs

import chisel3._
import dongjiang.frontend.decode.{ChiInst, CommitCode, Decode, StateInst, TaskCode, TaskInst}

// 译码表转储：把 Decode.parse 的四级内容寻址表原样拉到输出端口（每表拍平一路宽
// UInt；Vec.asUInt 元素 0 在最低位）。表由全局常量构成，与 cde 配置无关。
// 流程：refgenDj 出 SV → verilate 读端口 → 生成 model/dj/dj_decode_table.inc
// （等价于 firtool 看到的常量，零手翻风险）。
class DecodeDump extends Module {
  val ((chiInstVec, stateInstVec2, taskInstVec3, secInstVec4), (taskCodeVec2, secCodeVec3, commitCodeVec4)) =
    Decode.parse

  private val chiW = new ChiInst().getWidth
  private val siW  = new StateInst().getWidth
  private val tcW  = new TaskCode().getWidth
  private val tiW  = new TaskInst().getWidth
  private val ccW  = new CommitCode().getWidth

  val dims = IO(Output(Vec(8, UInt(16.W))))
  dims(0) := Decode.l_ci.U
  dims(1) := Decode.l_si.U
  dims(2) := Decode.l_ti.U
  dims(3) := Decode.l_sti.U
  dims(4) := chiW.U
  dims(5) := tcW.U
  dims(6) := tiW.U
  dims(7) := ccW.U

  val out_chi = IO(Output(UInt((Decode.l_ci * chiW).W)))
  val out_si  = IO(Output(UInt((Decode.l_ci * Decode.l_si * siW).W)))
  val out_tc  = IO(Output(UInt((Decode.l_ci * Decode.l_si * tcW).W)))
  val out_ti  = IO(Output(UInt((Decode.l_ci * Decode.l_si * Decode.l_ti * tiW).W)))
  val out_sc  = IO(Output(UInt((Decode.l_ci * Decode.l_si * Decode.l_ti * tcW).W)))
  val out_sti = IO(Output(UInt((Decode.l_ci * Decode.l_si * Decode.l_ti * Decode.l_sti * tiW).W)))
  val out_cc  = IO(Output(UInt((Decode.l_ci * Decode.l_si * Decode.l_ti * Decode.l_sti * ccW).W)))

  out_chi := chiInstVec.asUInt
  out_si  := stateInstVec2.asUInt
  out_tc  := taskCodeVec2.asUInt
  out_ti  := taskInstVec3.asUInt
  out_sc  := secCodeVec3.asUInt
  out_sti := secInstVec4.asUInt
  out_cc  := commitCodeVec4.asUInt
}

object DecodeDumpRef {
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("DecodeDump", () => new DecodeDump)
  )
}
