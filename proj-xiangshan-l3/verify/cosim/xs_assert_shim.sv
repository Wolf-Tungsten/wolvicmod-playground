// xs_assert_v2 桩（对拍专用）：emu 流程中它是 difftest 的 DPI-C 导入
// （difftest/src/test/vsrc/common/assert.v，由 Makefile 将 $fatal sed 替换而来）。
// 对拍侧断言失败不代表参考模型错（输入激励本就覆盖边角），打印后继续。
task xs_assert_v2(input string filename, input longint line);
  $display("[XS-ASSERT] %0s:%0d", filename, line);
endtask
