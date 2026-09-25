package refgen.refs

import chisel3._
import chisel3.util._
import dongjiang.utils.Alloc

// dongjiang Alloc（首个空闲项优先编码分配）的参考 wrapper。
class AllocRef(val n: Int) extends Module {
  override def desiredName = s"AllocRef_n$n"
  val in_valid  = IO(Input(Bool()))
  val in_bits   = IO(Input(UInt(32.W)))
  val in_ready  = IO(Output(Bool()))
  val out_valid = IO(Output(Vec(n, Bool())))  // UInt 输出位选择只读，用 Vec
  val out_bits  = IO(Output(Vec(n, UInt(32.W))))
  val out_ready = IO(Input(UInt(n.W)))
  val inW = Wire(Decoupled(UInt(32.W)))
  inW.valid := in_valid
  inW.bits  := in_bits
  in_ready  := inW.ready
  val outs = Seq.fill(n)(Wire(Decoupled(UInt(32.W))))
  Alloc(outs, inW)
  for (i <- 0 until n) {
    out_valid(i)   := outs(i).valid
    out_bits(i)    := outs(i).bits
    outs(i).ready  := out_ready(i)
  }
}

object AllocRef {
  // 对拍配置表：N=4 / 16
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("AllocRef_n4",  () => new AllocRef(4)),
    ("AllocRef_n16", () => new AllocRef(16)),
  )
}
