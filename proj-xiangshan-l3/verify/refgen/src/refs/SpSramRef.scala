package refgen.refs

import chisel3._
import chisel3.util._
import xs.utils.sram.SinglePortSramTemplate

// xs-utils SinglePortSramTemplate 的参考 wrapper（单口、way 掩码写）。
class SpSramRef(val set: Int, val way: Int, val shouldReset: Boolean,
                val setup: Int, val latency: Int, val outputReg: Boolean) extends Module {
  override def desiredName =
    s"SpSramRef_s${set}w${way}_r${if (shouldReset) 1 else 0}_su${setup}_l${latency}_o${if (outputReg) 1 else 0}"
  val req_valid  = IO(Input(Bool()))
  val req_addr   = IO(Input(UInt(log2Ceil(set).W)))
  val req_mask   = if (way > 1) Some(IO(Input(UInt(way.W)))) else None
  val req_write  = IO(Input(Bool()))
  val req_data   = IO(Input(Vec(way, UInt(32.W))))
  val req_ready  = IO(Output(Bool()))
  val resp_valid = IO(Output(Bool()))
  val resp_data  = IO(Output(Vec(way, UInt(32.W))))
  val sram = Module(new SinglePortSramTemplate(
    UInt(32.W), set = set, way = way,
    shouldReset = shouldReset, holdRead = false,
    setup = setup, latency = latency, extraHold = false,
    hasMbist = false, outputReg = outputReg))
  sram.io.req.valid      := req_valid
  sram.io.req.bits.addr  := req_addr
  sram.io.req.bits.write := req_write
  sram.io.req.bits.data  := req_data
  sram.io.req.bits.mask.foreach(_ := req_mask.get)
  req_ready  := sram.io.req.ready
  resp_valid := sram.io.resp.valid
  resp_data  := sram.io.resp.bits.data
}

object SpSramRef {
  // 对拍配置表：Directory(1,2,+outReg,ways=2)、BeatStorage(2,2,+outReg,ways=1)、
  //             basic(1,1,无 outReg,ways=2)、shouldReset(1,1,+outReg,sets=4)
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("SpSramRef_s16w2_r0_su1_l2_o1", () => new SpSramRef(16, 2, false, 1, 2, true)),
    ("SpSramRef_s16w1_r0_su2_l2_o1", () => new SpSramRef(16, 1, false, 2, 2, true)),
    ("SpSramRef_s16w2_r0_su1_l1_o0", () => new SpSramRef(16, 2, false, 1, 1, false)),
    ("SpSramRef_s4w1_r1_su1_l1_o1",  () => new SpSramRef(4, 1, true, 1, 1, true)),
  )
}
