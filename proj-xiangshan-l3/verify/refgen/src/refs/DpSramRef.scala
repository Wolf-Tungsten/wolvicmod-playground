package refgen.refs

import chisel3._
import chisel3.util._
import xs.utils.sram.DualPortSramTemplate

// xs-utils DualPortSramTemplate 的参考 wrapper（1R1W，可选同址写前递）。
class DpSramRef(val set: Int, val way: Int, val bypass: Boolean,
                val setup: Int, val latency: Int, val outputReg: Boolean) extends Module {
  override def desiredName =
    s"DpSramRef_s${set}w${way}_b${if (bypass) 1 else 0}_su${setup}_l${latency}_o${if (outputReg) 1 else 0}"
  val wreq_valid  = IO(Input(Bool()))
  val wreq_addr   = IO(Input(UInt(log2Ceil(set).W)))
  val wreq_mask   = if (way > 1) Some(IO(Input(UInt(way.W)))) else None
  val wreq_data   = IO(Input(Vec(way, UInt(32.W))))
  val wreq_ready  = IO(Output(Bool()))
  val rreq_valid  = IO(Input(Bool()))
  val rreq_addr   = IO(Input(UInt(log2Ceil(set).W)))
  val rreq_ready  = IO(Output(Bool()))
  val rresp_valid = IO(Output(Bool()))
  val rresp_data  = IO(Output(Vec(way, UInt(32.W))))
  val sram = Module(new DualPortSramTemplate(
    UInt(32.W), set = set, way = way,
    shouldReset = false, holdRead = false, bypassWrite = bypass,
    setup = setup, latency = latency, extraHold = false,
    hasMbist = false, outputReg = outputReg))
  sram.io.wreq.valid      := wreq_valid
  sram.io.wreq.bits.addr  := wreq_addr
  sram.io.wreq.bits.data  := wreq_data
  sram.io.wreq.bits.mask.foreach(_ := wreq_mask.get)
  wreq_ready := sram.io.wreq.ready
  sram.io.rreq.valid := rreq_valid
  sram.io.rreq.bits  := rreq_addr
  rreq_ready  := sram.io.rreq.ready
  rresp_valid := sram.io.rresp.valid
  rresp_data  := sram.io.rresp.bits
}

object DpSramRef {
  // 对拍配置表：replArray(bypass,1,1,+outReg,ways=1)、(nobypass)、(bypass,ways=2)
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("DpSramRef_s16w1_b1_su1_l1_o1", () => new DpSramRef(16, 1, true,  1, 1, true)),
    ("DpSramRef_s16w1_b0_su1_l1_o1", () => new DpSramRef(16, 1, false, 1, 1, true)),
    ("DpSramRef_s16w2_b1_su1_l1_o1", () => new DpSramRef(16, 2, true,  1, 1, true)),
  )
}
