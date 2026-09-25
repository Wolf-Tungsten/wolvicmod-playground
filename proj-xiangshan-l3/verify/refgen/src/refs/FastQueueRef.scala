package refgen.refs

import chisel3._
import chisel3.util._
import xs.utils.queue.FastQueue

// xs-utils FastQueue 的参考 wrapper（移位队列、enq.ready 寄存）。
class FastQueueRef(val size: Int, val noX: Boolean) extends Module {
  override def desiredName = s"FastQueueRef_s${size}_x${if (noX) 1 else 0}"
  val enq_valid = IO(Input(Bool()))
  val enq_bits  = IO(Input(UInt(32.W)))
  val enq_ready = IO(Output(Bool()))
  val deq_valid = IO(Output(Bool()))
  val deq_bits  = IO(Output(UInt(32.W)))
  val deq_ready = IO(Input(Bool()))
  val count     = IO(Output(UInt(log2Ceil(size + 1).W)))
  val free_num  = IO(Output(UInt(log2Ceil(size + 1).W)))
  val q = Module(new FastQueue(UInt(32.W), size, noX))
  q.io.enq.valid := enq_valid
  q.io.enq.bits  := enq_bits
  enq_ready := q.io.enq.ready
  deq_valid := q.io.deq.valid
  deq_bits  := q.io.deq.bits
  q.io.deq.ready := deq_ready
  count    := q.io.count
  free_num := q.io.freeNum
}

object FastQueueRef {
  // 对拍配置表：N=2 / N=4（NoX=false）、N=2 NoX=true
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("FastQueueRef_s2_x0", () => new FastQueueRef(2, false)),
    ("FastQueueRef_s4_x0", () => new FastQueueRef(4, false)),
    ("FastQueueRef_s2_x1", () => new FastQueueRef(2, true)),
  )
}
