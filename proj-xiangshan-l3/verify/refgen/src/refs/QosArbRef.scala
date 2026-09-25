package refgen.refs

import chisel3._
import chisel3.util._
import dongjiang.utils.ArbiterGenerator

class QosBits extends Bundle {
  val qos  = UInt(4.W)
  val data = UInt(32.W)
}

// dongjiang FastArb 的 ArbiterGenerator（rr=true → fastQosRRArb，rr=false → fastQosArb）。
class QosArbRef(val n: Int, val rr: Boolean) extends Module {
  override def desiredName = s"QosArbRef_n${n}_${if (rr) "rr" else "fx"}"
  val in_valid  = IO(Input(UInt(n.W)))
  val in_qos    = IO(Input(Vec(n, UInt(4.W))))
  val in_data   = IO(Input(Vec(n, UInt(32.W))))
  val in_ready  = IO(Output(Vec(n, Bool())))  // UInt 输出位选择只读，用 Vec
  val out_valid = IO(Output(Bool()))
  val out_qos   = IO(Output(UInt(4.W)))
  val out_data  = IO(Output(UInt(32.W)))
  val out_ready = IO(Input(Bool()))
  val arb = Module(new ArbiterGenerator(new QosBits, n, rr, true))
  for (i <- 0 until n) {
    arb.io.in(i).valid     := in_valid(i)
    arb.io.in(i).bits.qos  := in_qos(i)
    arb.io.in(i).bits.data := in_data(i)
    in_ready(i)            := arb.io.in(i).ready
  }
  out_valid := arb.io.out.valid
  out_qos   := arb.io.out.bits.qos
  out_data  := arb.io.out.bits.data
  arb.io.out.ready := out_ready
}

object QosArbRef {
  // 对拍配置表：N=4，rr / fixed
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("QosArbRef_n4_rr", () => new QosArbRef(4, rr = true)),
    ("QosArbRef_n4_fx", () => new QosArbRef(4, rr = false)),
  )
}
