package refgen.dj

import chisel3._
import _root_.circt.stage.ChiselStage

// DongJiang 对拍参考源生成驱动（P3）。各模块 wrapper 与配置表在 srcdj/refs/ 下
// 的独立文件中：DirectoryRef（后续 DataBlockRef / BackendRef / FrontendRef ...）。
object DjRefGen extends App {
  // 与 RefGen 相同的 firtool 选项（XiangShan Makefile:109 同流程）
  private val firtoolOpts = Array(
    "-O=release",
    "--disable-annotation-unknown",
    "--lowering-options=explicitBitcast,disallowLocalVariables,disallowPortDeclSharing,locationInfoStyle=none",
  )

  val outDir = args(0)
  val filter = if (args.length > 1) Some(args(1)) else None
  val firtoolPath = firtoolresolver.Resolve(chisel3.BuildInfo.firtoolVersion.get, true) match {
    case Right(bin) => bin.path.getAbsolutePath
    case Left(err)  => throw new RuntimeException(s"firtool resolve failed: $err")
  }
  println(s"[refgen-dj] firtool: $firtoolPath")
  def emit(gen: => RawModule): Unit = {
    ChiselStage.emitSystemVerilogFile(
      gen,
      Array("--target-dir", outDir, "--firtool-binary-path", firtoolPath),
      firtoolOpts,
    )
  }
  def emitIf(name: String)(gen: => RawModule): Unit =
    if (filter.forall(name.contains)) emit(gen)

  private val all: Seq[(String, () => RawModule)] =
    refs.DirectoryRef.configs ++ refs.DataBlockRef.configs ++ refs.BackendRef.configs ++
      refs.FrontendRef.configs ++ refs.ChiXbarRef.configs ++
      refs.GetDecResProbeRef.configs ++ refs.SecDecProbeRef.configs ++ refs.DecodeDumpRef.configs
  for ((name, gen) <- all) emitIf(name)(gen())
  println(s"[refgen-dj] done -> $outDir")
}
