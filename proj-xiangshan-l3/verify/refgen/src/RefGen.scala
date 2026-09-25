package refgen

import chisel3._
import _root_.circt.stage.ChiselStage

// 对拍参考源生成驱动。各模块的 wrapper 与配置表在 src/refs/ 下的独立文件中：
// FastQueueRef / VipArbRef / QosArbRef / AllocRef / SpSramRef / DpSramRef。
object RefGen extends App {
  // XiangShan Makefile:109 的 firtool 选项（与 kunminghu-v3 emu 同流程）
  private val firtoolOpts = Array(
    "-O=release",
    "--disable-annotation-unknown",
    "--lowering-options=explicitBitcast,disallowLocalVariables,disallowPortDeclSharing,locationInfoStyle=none",
  )

  val outDir = args(0)
  val filter = if (args.length > 1) Some(args(1)) else None  // 名称子串过滤（run.sh 逐配置进程用）
  val firtoolPath = firtoolresolver.Resolve(chisel3.BuildInfo.firtoolVersion.get, true) match {
    case Right(bin) => bin.path.getAbsolutePath
    case Left(err)  => throw new RuntimeException(s"firtool resolve failed: $err")
  }
  println(s"[refgen] firtool: $firtoolPath")
  // 注意：xs-utils SramProto 的 defMap 是 JVM 全局缓存，Definition 不能跨
  // elaboration 复用——SRAM 类配置由 run.sh 每配置一个进程单独发射。
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
    refs.FastQueueRef.configs ++ refs.VipArbRef.configs ++ refs.QosArbRef.configs ++
      refs.AllocRef.configs ++ refs.SpSramRef.configs ++ refs.DpSramRef.configs
  for ((name, gen) <- all) emitIf(name)(gen())
  println(s"[refgen] done -> $outDir")
}
