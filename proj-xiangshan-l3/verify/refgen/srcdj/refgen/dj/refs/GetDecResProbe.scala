package refgen.dj.refs

import chisel3._
import dongjiang.backend.GetDecRes
import dongjiang.frontend.decode.Decode._

class GetDecResProbe extends Module {
  implicit val p: org.chipsalliance.cde.config.Parameters = DjConfig()
  private def probe(ci: Int, si: Int, ti: Int, sti: Int): (UInt, UInt, UInt) = {
    val g = Module(new GetDecRes())
    g.io.list.valid := true.B
    g.io.list.bits(0) := ci.U
    g.io.list.bits(1) := si.U
    g.io.list.bits(2) := ti.U
    g.io.list.bits(3) := sti.U
    (g.io.taskCode.asUInt, g.io.secTaskCode.asUInt, g.io.commitCode.asUInt)
  }
  val out = IO(Output(Vec(6, Vec(3, UInt(29.W)))))
  val (t0a, t0b, t0c) = probe(0, 0, 0, 0)
  val (t1a, t1b, t1c) = probe(3, 1, 0, 0)
  val (t2a, t2b, t2c) = probe(3, 1, 6, 0)
  val (t3a, t3b, t3c) = probe(3, 4, 2, 0)
  val (t4a, t4b, t4c) = probe(3, 4, 7, 0)
  val (t5a, t5b, t5c) = probe(3, 4, 4, 0)
  out(0) := VecInit(Seq(t0a, t0b, t0c))
  out(1) := VecInit(Seq(t1a, t1b, t1c))
  out(2) := VecInit(Seq(t2a, t2b, t2c))
  out(3) := VecInit(Seq(t3a, t3b, t3c))
  out(4) := VecInit(Seq(t4a, t4b, t4c))
  out(5) := VecInit(Seq(t5a, t5b, t5c))
}

object GetDecResProbeRef {
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("GetDecResProbe", () => new GetDecResProbe)
  )
}
